#!/usr/bin/env bash
# MicroHTTP correctness tests for Phases 3, 4 and 5.
# Run from the project root:   make test   (or: bash tests/test_http.sh)
# Uses only bash, curl, and common coreutils.

set -u

PORT="${PORT:-18080}"
BIN="build/microhttp"
BASE="http://127.0.0.1:${PORT}"
PASS=0
FAIL=0
SERVER_PID=""
FIRST_PID=""

ok()  { PASS=$((PASS + 1)); printf 'PASS  %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf 'FAIL  %s   expected="%s" got="%s"\n' "$1" "$2" "$3"; }
check_eq() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "$2" "$3"; fi; }

code() { curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$@"; }
hdr()  { curl -s -D - -o /dev/null --max-time 5 "$@" | tr -d '\r'; }
header_val() { # header_val HEADER_NAME  (reads stdin = headers)
    awk -F': ' -v h="$(printf '%s' "$1" | tr 'A-Z' 'a-z')" \
        'tolower($1) == h { print $2 }'
}

[ -x "$BIN" ] || { echo "binary not found - run make first"; exit 1; }
[ -f www/index.html ] || { echo "www/index.html missing"; exit 1; }

# ---------- fixtures (removed again on exit) ----------
mkdir -p www/testdir
printf 'body { color: red; }\n'                > www/style.css
printf 'hello microhttp\n'                     > www/test.txt
printf '<!DOCTYPE html>\n<html><body>test page</body></html>\n' > www/test_page.html
printf '\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x01\x02\xff'       > www/tiny.png
ln -sf /etc/passwd www/evil_link

cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    [ -n "$FIRST_PID" ] && kill "$FIRST_PID" 2>/dev/null
    rm -f www/style.css www/test.txt www/test_page.html www/tiny.png www/evil_link
    rm -f www/dyn_test.html
    rmdir www/testdir 2>/dev/null
    rm -f /tmp/mh_tiny_out.png /tmp/mh_codes.txt /tmp/mh_arg.out /tmp/mh_noroot.out
    rm -f /tmp/mh_first.out /tmp/mh_second.out
}
trap cleanup EXIT

# ---------- start server ----------
# Capacity: 8 workers + 64 queue = 72 concurrent slots. The 30-client
# burst below always fits (deterministic all-200); the 90 stalled
# connections later exceed capacity to exercise queue-full rejection.
"$BIN" -p "$PORT" -t 8 -q 64 > /tmp/microhttp_test_server.log 2>&1 &
SERVER_PID=$!

for i in $(seq 1 30); do
    curl -s -o /dev/null --max-time 1 "$BASE/" && break
    sleep 0.2
done

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "server failed to start:"; cat /tmp/microhttp_test_server.log; exit 1
fi

echo "== A. Basic HTTP =="

check_eq "GET / is 200"                    "200" "$(code "$BASE/")"

CTYPE=$(hdr "$BASE/" | header_val "Content-Type")
check_eq "MIME for / (index.html)" "text/html; charset=utf-8" "$CTYPE"
check_eq "GET /test_page.html is 200"      "200" "$(code "$BASE/test_page.html")"
check_eq "GET /missing.html is 404"        "404" "$(code "$BASE/missing.html")"
check_eq "GET /testdir (directory) is 404" "404" "$(code "$BASE/testdir")"

CTYPE=$(hdr "$BASE/style.css" | header_val "Content-Type")
check_eq "MIME for .css" "text/css; charset=utf-8" "$CTYPE"

CTYPE=$(hdr "$BASE/tiny.png" | header_val "Content-Type")
check_eq "MIME for .png" "image/png" "$CTYPE"

CTYPE=$(hdr "$BASE/test.txt" | header_val "Content-Type")
check_eq "MIME for .txt" "text/plain; charset=utf-8" "$CTYPE"

# HEAD: same Content-Length as GET, but no body.
# (printf 'x' marker: $() strips trailing newlines, which would eat the
# final \r\n\r\n and break the body check below)
CL_GET=$(hdr "$BASE/" | header_val "Content-Length")
HEAD_RAW=$( (exec 3<>/dev/tcp/127.0.0.1/"$PORT" &&
             printf 'HEAD / HTTP/1.1\r\nHost: t\r\n\r\n' >&3 &&
             cat <&3; printf 'x'; exec 3>&-) 2>/dev/null )
HEAD_RAW="${HEAD_RAW%x}"
CL_HEAD=$(printf '%s' "$HEAD_RAW" | tr -d '\r' | header_val "Content-Length")
HEAD_BODY="${HEAD_RAW#*$'\r\n\r\n'}"
check_eq "HEAD Content-Length == GET" "$CL_GET" "$CL_HEAD"
check_eq "HEAD sends no body"         ""         "$HEAD_BODY"

# 405 with Allow header
check_eq "POST is 405" "405" "$(code -X POST "$BASE/")"
ALLOW=$(hdr -X POST "$BASE/" | header_val "Allow")
check_eq "405 has Allow: GET, HEAD" "GET, HEAD" "$ALLOW"

# binary round trip
curl -s --max-time 5 -o /tmp/mh_tiny_out.png "$BASE/tiny.png"
if cmp -s www/tiny.png /tmp/mh_tiny_out.png; then
    ok "binary file byte-identical"
else
    bad "binary file byte-identical" "same bytes" "different bytes"
fi

echo "== B. Security =="

check_eq "traversal /../secret.txt"        "400" "$(code --path-as-is "$BASE/../secret.txt")"
check_eq "traversal /../../etc/passwd"     "400" "$(code --path-as-is "$BASE/../../etc/passwd")"
check_eq "encoded %2e%2e"                  "400" "$(code --path-as-is "$BASE/%2e%2e/etc/passwd")"
check_eq "encoded %2E%2E%2F"               "400" "$(code --path-as-is "$BASE/%2E%2E%2Fetc/passwd")"
check_eq "malformed percent %zz"           "400" "$(code --path-as-is "$BASE/%zz")"
check_eq "NUL byte %00"                    "400" "$(code --path-as-is "$BASE/%00")"
check_eq "/etc/passwd stays in web root"   "404" "$(code --path-as-is "$BASE/etc/passwd")"
check_eq "symlink escape is 404"           "404" "$(code "$BASE/evil_link")"

RAW=$( (exec 3<>/dev/tcp/127.0.0.1/"$PORT" &&
        printf 'GARBAGE\r\n\r\n' >&3 && cat <&3; exec 3>&-) 2>/dev/null )
case "$RAW" in
    *"400 Bad Request"*) ok "malformed request is 400" ;;
    *) bad "malformed request is 400" "400 Bad Request" "$RAW" ;;
esac

# oversized: 8500-byte request line area with no \r\n\r\n terminator
BIG=$(head -c 8500 /dev/zero | tr '\0' 'a')
RAW=$( (exec 3<>/dev/tcp/127.0.0.1/"$PORT" &&
        printf 'GET / HTTP/1.1\r\nX-Big: %s' "$BIG" >&3 &&
        cat <&3; exec 3>&-) 2>/dev/null )
case "$RAW" in
    *"400 Bad Request"*) ok "oversized request is 400" ;;
    *) bad "oversized request is 400" "400 Bad Request" "$(printf '%.60s' "$RAW")" ;;
esac

echo "== C. Concurrency =="

# 30 simultaneous clients (worker count is 4, queue is 8).
# Wait on the curl PIDs explicitly - a bare "wait" would also wait for
# the server process, which never exits on its own.
PIDS=""
for i in $(seq 1 30); do
    curl -s -o /dev/null -w '%{http_code}\n' --max-time 10 "$BASE/" &
    PIDS="$PIDS $!"
done > /tmp/mh_codes.txt
for p in $PIDS; do wait "$p" 2>/dev/null; done

N=$(wc -l < /tmp/mh_codes.txt | tr -d ' ')
U=$(sort -u /tmp/mh_codes.txt | tr -d ' \n')
check_eq "30 parallel requests answered" "30" "$N"
check_eq "all parallel requests were 200" "200" "$U"

# one stalled client must not block the other workers.
# Let it close on its own (sleep 2 < server timeout) - no kill needed.
(exec 3<>/dev/tcp/127.0.0.1/"$PORT"; sleep 2; exec 3>&-) &
SLOW=$!
check_eq "normal request while 1 client stalled" "200" "$(code "$BASE/")"
wait "$SLOW" 2>/dev/null

# flood with more stalled connections than workers+queue (90 > 72);
# server must survive, reject the excess, and recover after timeouts
for i in $(seq 1 90); do
    (exec 3<>/dev/tcp/127.0.0.1/"$PORT"; sleep 6; exec 3>&-) &
done
sleep 1
if kill -0 "$SERVER_PID" 2>/dev/null; then
    ok "server alive with 90 stalled clients"
else
    bad "server alive with 90 stalled clients" "alive" "crashed"
fi
sleep 8
check_eq "service recovers after stalled clients time out" "200" "$(code "$BASE/")"

echo "== D. Shutdown =="

kill -TERM "$SERVER_PID"
DEAD=1
for i in $(seq 1 25); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then DEAD=0; break; fi
    sleep 0.2
done
check_eq "SIGTERM stops the server promptly" "0" "$DEAD"
wait "$SERVER_PID" 2>/dev/null
RC=$?
SERVER_PID=""
check_eq "exit status after SIGTERM is 0" "0" "$RC"

# restart must work (no leaked port / no crash on second start)
"$BIN" -p "$PORT" -t 4 -q 8 >> /tmp/microhttp_test_server.log 2>&1 &
SERVER_PID=$!
for i in $(seq 1 30); do
    curl -s -o /dev/null --max-time 1 "$BASE/" && break
    sleep 0.2
done
check_eq "restart after shutdown works" "200" "$(code "$BASE/")"

kill -INT "$SERVER_PID"
DEAD=1
for i in $(seq 1 25); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then DEAD=0; break; fi
    sleep 0.2
done
check_eq "SIGINT stops the server promptly" "0" "$DEAD"
wait "$SERVER_PID" 2>/dev/null
RC=$?
SERVER_PID=""
check_eq "exit status after SIGINT is 0" "0" "$RC"

echo "== E. Regression (Phase 1/2) =="
# Section D stopped the server - start a fresh one for regression checks
"$BIN" -p "$PORT" -t 4 -q 8 >> /tmp/microhttp_test_server.log 2>&1 &
SERVER_PID=$!
for i in $(seq 1 30); do
    curl -s -o /dev/null --max-time 1 "$BASE/" && break
    sleep 0.2
done
check_eq "GET / still 200 after all changes" "200" "$(code "$BASE/")"
check_eq "HTTP/1.0 request still served"    "200" "$(code --http1.0 "$BASE/")"

# Stop the section-E server so section F starts with a free port;
# otherwise FIRST_PID's bind fails silently and its checks would
# pass against this (wrong) process.
kill -TERM "$SERVER_PID"
for i in $(seq 1 25); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then break; fi
    sleep 0.2
done
wait "$SERVER_PID" 2>/dev/null
SERVER_PID=""

echo "== F. Reliability (arguments, startup failures, dynamic pages) =="

# Invalid command-line arguments must be rejected before binding.
# Runs in the background with a bounded wait so a wrongly-accepted
# argument cannot hang the suite.
expect_reject() {
    # expect_reject "description" args...
    local desc="$1"; shift
    "$BIN" "$@" > /tmp/mh_arg.out 2>&1 &
    local pid=$!
    local exited=0 rc=-1
    for _ in $(seq 1 25); do
        if ! kill -0 "$pid" 2>/dev/null; then exited=1; break; fi
        sleep 0.2
    done
    if [ "$exited" -eq 1 ]; then
        wait "$pid"; rc=$?
        if [ "$rc" -ne 0 ] && [ "$rc" -lt 126 ]; then
            ok "$desc"
        else
            bad "$desc" "non-zero clean exit" "rc=$rc out=$(head -c 80 /tmp/mh_arg.out)"
        fi
    else
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        bad "$desc" "rejected promptly" "server kept running"
    fi
}

expect_reject "-p abc rejected"        -p abc
expect_reject "-p 99999 rejected"      -p 99999
expect_reject "-p 0 rejected"          -p 0
expect_reject "-t 0 rejected"          -t 0
expect_reject "-t 999 rejected"        -t 999
expect_reject "-q 0 rejected"          -q 0
expect_reject "-p (empty) rejected"    -p ""
expect_reject "unknown flag rejected"  -z
expect_reject "extra positional arg rejected" extra
expect_reject "-p 80abc rejected (strict parse)" -p 80abc

# Missing document root: run from a directory without www/.
# Bounded wait: if the server wrongly starts, kill it instead of hanging.
PROJ_DIR="$(pwd)"
(cd /tmp && exec "$PROJ_DIR/$BIN" -p 19998) > /tmp/mh_noroot.out 2>&1 &
NOROOT_PID=$!
NOROOT_EXITED=0
for i in $(seq 1 30); do
    if ! kill -0 "$NOROOT_PID" 2>/dev/null; then NOROOT_EXITED=1; break; fi
    sleep 0.2
done
if [ "$NOROOT_EXITED" -eq 1 ]; then
    wait "$NOROOT_PID"
    RC=$?
    if [ "$RC" -ne 0 ] && [ "$RC" -lt 126 ]; then
        ok "missing document root exits non-zero"
    else
        bad "missing document root exits non-zero" "non-zero clean exit" "rc=$RC"
    fi
else
    kill "$NOROOT_PID" 2>/dev/null
    wait "$NOROOT_PID" 2>/dev/null
    bad "missing document root exits non-zero" "exited promptly" "still running"
fi

# Port already in use: second server must fail to bind and exit.
"$BIN" -p "$PORT" -t 2 -q 4 > /tmp/mh_first.out 2>&1 &
FIRST_PID=$!
FIRST_UP=0
for i in $(seq 1 30); do
    if ! kill -0 "$FIRST_PID" 2>/dev/null; then break; fi
    if curl -s -o /dev/null --max-time 1 "$BASE/"; then FIRST_UP=1; break; fi
    sleep 0.2
done
if [ "$FIRST_UP" -ne 1 ]; then
    echo "FATAL: first server did not come up:"; cat /tmp/mh_first.out; exit 1
fi
"$BIN" -p "$PORT" -t 2 -q 4 > /tmp/mh_second.out 2>&1 &
SECOND_PID=$!
SECOND_EXITED=0
for i in $(seq 1 25); do
    if ! kill -0 "$SECOND_PID" 2>/dev/null; then SECOND_EXITED=1; break; fi
    sleep 0.2
done
if [ "$SECOND_EXITED" -eq 1 ]; then
    wait "$SECOND_PID"
    RC=$?
    if [ "$RC" -ne 0 ] && [ "$RC" -lt 126 ]; then
        ok "bind failure on occupied port exits non-zero"
    else
        bad "bind failure on occupied port exits non-zero" "non-zero clean exit" "rc=$RC"
    fi
else
    kill "$SECOND_PID" 2>/dev/null
    wait "$SECOND_PID" 2>/dev/null
    bad "bind failure on occupied port exits non-zero" "failed to bind" "kept running"
fi

# Dynamic page: new file in www/ is served without recompilation
# (server above is still running).
printf '<!DOCTYPE html>\n<html><body>dyn</body></html>\n' > www/dyn_test.html
check_eq "new page /dyn_test.html is 200 (no restart)" "200" "$(code "$BASE/dyn_test.html")"
rm -f www/dyn_test.html
check_eq "removed page is 404 again" "404" "$(code "$BASE/dyn_test.html")"

echo "== G. Shutdown under active load =="

# Shutdown while a worker is mid-request must still finish in bounded time.
(exec 3<>/dev/tcp/127.0.0.1/"$PORT"; printf 'GET / HTTP/1.1\r\nHost: t\r\n\r\n' >&3; sleep 3; exec 3>&-) &
MID_PID=$!
sleep 0.3
kill -TERM "$FIRST_PID"
DEAD=1
for i in $(seq 1 40); do
    if ! kill -0 "$FIRST_PID" 2>/dev/null; then DEAD=0; break; fi
    sleep 0.2
done
check_eq "shutdown under active load completes" "0" "$DEAD"
wait "$FIRST_PID" 2>/dev/null
RC=$?
FIRST_PID=""
check_eq "exit 0 under load" "0" "$RC"
wait "$MID_PID" 2>/dev/null

echo "-----------------------------------------"
printf 'TOTAL  pass=%d  fail=%d\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
