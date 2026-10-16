#!/usr/bin/env bash
# MicroHTTP reproducible benchmark (Phase 12).
#
# Measures, for each (workers, concurrency) pair:
#   - throughput (requests/second)
#   - median and p95 latency (ms)
#   - error rate (non-200 responses)
#   - server CPU% and RSS (ps snapshot, Linux-friendly)
#
# Usage:   bash bench/bench.sh [requests] [concurrency_list] [worker_list]
# Example: bash bench/bench.sh 500 "1 10 50" "1 4 8"
#
# Requires: bash, curl, awk, sort, ps. No external benchmark tool.
# The same request is replayed against / (index.html) every time so
# the workload is identical across all runs.

set -u

N="${1:-200}"
CONCURRENCIES="${2:-1 10 40}"
WORKERS="${3:-1 4 8}"
PORT="${PORT:-18100}"
BIN="build/microhttp"
LOG="/tmp/mh_bench_server.log"
LAT="/tmp/mh_bench_lat.txt"
SERVER_PID=""

[ -x "$BIN" ] || { echo "run make first"; exit 1; }

cleanup() {
    [ -n "${SERVER_PID:-}" ] && kill "$SERVER_PID" 2>/dev/null
    rm -f "$LAT"
}
trap cleanup EXIT

# High-resolution clock (GNU date %N is unavailable on macOS/BSD).
if DATE_NS=$(date +%s.%N 2>/dev/null) && [ "${DATE_NS#*%}" = "$DATE_NS" ]; then
    now() { date +%s.%N; }
elif perl -MTime::HiRes=time -e 'exit 0' 2>/dev/null; then
    now() { perl -MTime::HiRes=time -e 'printf "%.3f\n", time'; }
else
    now() { date +%s; }   # 1-second resolution fallback
fi

# percentile FILE PCT  (PCT = 1..99, input: one latency per line, ms)
percentile() {
    sort -n "$1" | awk -v p="$2" '
        { a[NR] = $1 }
        END {
            if (NR == 0) { print "n/a"; exit }
            idx = int((NR * p + 99) / 100)
            if (idx < 1) idx = 1
            if (idx > NR) idx = NR
            printf "%.1f", a[idx]
        }'
}

mean_latency() {
    awk '{ s += $1; n++ } END { if (n) printf "%.1f", s/n; else print "n/a" }' "$1"
}

echo "=== MicroHTTP benchmark ==="
echo "date        : $(date)"
echo "host        : $(uname -srm)"
echo "requests    : $N per cell"
echo "concurrency : $CONCURRENCIES"
echo "workers     : $WORKERS"
echo

printf '%-8s %-6s %10s %10s %10s %10s %9s %9s\n' \
       "workers" "conc" "req/s" "median_ms" "p95_ms" "mean_ms" "errors" "rss_mb"

for W in $WORKERS; do
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    SERVER_PID=""

    "$BIN" -p "$PORT" -t "$W" -q 256 > "$LOG" 2>&1 &
    SERVER_PID=$!

    up=0
    for i in $(seq 1 30); do
        curl -s -o /dev/null --max-time 1 "http://127.0.0.1:${PORT}/" && { up=1; break; }
        sleep 0.2
    done
    if [ "$up" -ne 1 ]; then
        echo "server failed to start (workers=$W):"; cat "$LOG"; exit 1
    fi

    # warm-up (untimed): 20 requests
    for i in $(seq 1 20); do
        curl -s -o /dev/null "http://127.0.0.1:${PORT}/"
    done

    RSS_KB=$(ps -o rss= -p "$SERVER_PID" 2>/dev/null | tr -d ' ')

    for C in $CONCURRENCIES; do
        : > "$LAT"
        ERR=0

        START=$(now)
        PIDS=""
        REMAIN=$N
        while [ "$REMAIN" -gt 0 ]; do
            BATCH=$C
            [ "$BATCH" -gt "$REMAIN" ] && BATCH=$REMAIN
            BPIDS=""
            for i in $(seq 1 "$BATCH"); do
                curl -s -o /dev/null -w '%{time_total}\n' \
                     --max-time 30 "http://127.0.0.1:${PORT}/" >> "$LAT" &
                BPIDS="$BPIDS $!"
            done
            for p in $BPIDS; do wait "$p" || ERR=$((ERR + 1)); done
            REMAIN=$((REMAIN - BATCH))
        done
        END=$(now)

        ELAPSED=$(awk -v a="$START" -v b="$END" 'BEGIN { printf "%.3f", b - a }')
        RPS=$(awk -v n="$N" -v e="$ELAPSED" 'BEGIN { if (e > 0) printf "%.0f", n / e; else print "n/a" }')
        # time_total is seconds; convert to ms
        awk '{ printf "%.2f\n", $1 * 1000 }' "$LAT" > "${LAT}.ms"
        MED=$(percentile "${LAT}.ms" 50)
        P95=$(percentile "${LAT}.ms" 95)
        MEAN=$(mean_latency "${LAT}.ms")
        RSS_MB=$(awk -v k="${RSS_KB:-0}" 'BEGIN { printf "%.1f", k / 1024 }')

        printf '%-8s %-6s %10s %10s %10s %10s %9s %9s\n' \
               "$W" "$C" "$RPS" "$MED" "$P95" "$MEAN" "$ERR" "$RSS_MB"
    done

    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=""
done

echo
echo "notes:"
echo " - errors = curl transfers that did not complete successfully"
echo " - latency = curl time_total (connection + request + full response)"
echo " - rss sampled once per workers-setting, before the concurrency sweep"
echo " - results are only meaningful on an idle machine; re-run 3x and compare"
