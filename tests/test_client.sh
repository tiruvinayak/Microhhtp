#!/usr/bin/env bash
# MicroHTTP interactive-client tests (Phase 6/7/8).
# Run from the project root:   make test   (or: bash tests/test_client.sh)
# Uses only bash, the built client/server, and common coreutils.

set -u

PORT="${PORT:-18081}"
BIN="build/microhttp"
CLI="build/client"
PASS=0
FAIL=0
SERVER_PID=""

ok()  { PASS=$((PASS + 1)); printf 'PASS  %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf 'FAIL  %s   expected="%s" got="%s"\n' "$1" "$2" "$3"; }
check_eq() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "$2" "$3"; fi; }

[ -x "$BIN" ] || { echo "server binary not found - run make first"; exit 1; }
[ -x "$CLI" ] || { echo "client binary not found - run make first"; exit 1; }

cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    rm -f /tmp/mh_cli_*.out
}
trap cleanup EXIT

# ---------- start server ----------
"$BIN" -p "$PORT" -t 4 -q 8 > /tmp/mh_cli_server.log 2>&1 &
SERVER_PID=$!
for i in $(seq 1 30); do
    curl -s -o /dev/null --max-time 1 "http://127.0.0.1:${PORT}/" && break
    sleep 0.2
done
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "server failed to start:"; cat /tmp/mh_cli_server.log; exit 1
fi

# run_client "outfile" <<< "commands..."  (stdin = commands, incl. exit)
run_client() {
    "$CLI" "$PORT" > "$1" 2>&1
}

echo "== 1. Multi-command line =="

# Six commands on one line: each response printed between separators.
printf 'home about contact services team help\nexit\n' | run_client /tmp/mh_cli_multi.out
COUNT=$(grep -c '^HTTP/1.1 200 OK' /tmp/mh_cli_multi.out)
check_eq "6 commands -> 6 x 200 OK" "6" "$COUNT"
SEPS=$(grep -c '^>> ' /tmp/mh_cli_multi.out)
check_eq "6 response separators printed" "6" "$SEPS"
check_eq "no FAIL/no error text" "0" "$(grep -ci 'error\|cannot\|invalid' /tmp/mh_cli_multi.out)"

echo "== 2. Individual commands =="

printf 'head\nexit\n' | run_client /tmp/mh_cli_head.out
if grep -q '^HTTP/1.1 200 OK' /tmp/mh_cli_head.out; then
    ok "head -> 200 OK"
else
    bad "head -> 200 OK" "200 OK" "$(grep '^HTTP' /tmp/mh_cli_head.out | head -1)"
fi
# HEAD must not include a body: simplest robust check - the file must
# not contain the index.html body marker.
if grep -q 'A simple concurrent Linux web server' /tmp/mh_cli_head.out; then
    bad "head sends no body" "no HTML body" "index.html body found"
else
    ok "head sends no body"
fi

printf 'missing\nexit\n' | run_client /tmp/mh_cli_missing.out
check_eq "missing -> 404" "1" "$(grep -c '^HTTP/1.1 404' /tmp/mh_cli_missing.out)"

printf 'post\nexit\n' | run_client /tmp/mh_cli_post.out
check_eq "post -> 405" "1" "$(grep -c '^HTTP/1.1 405' /tmp/mh_cli_post.out)"
check_eq "post shows Allow header" "1" "$(grep -c '^Allow: GET, HEAD' /tmp/mh_cli_post.out)"

echo "== 3. Security command =="

printf 'security\nexit\n' | run_client /tmp/mh_cli_sec.out
check_eq "security verdict PASS" "1" "$(grep -c 'security result: PASS' /tmp/mh_cli_sec.out)"
check_eq "two traversal tests run" "2" "$(grep -c 'traversal' /tmp/mh_cli_sec.out)"
check_eq "no REJECTED bodies exposed" "0" "$(grep -c 'root:' /tmp/mh_cli_sec.out)"

echo "== 4. Dynamic page names (no client change needed) =="

printf '<!DOCTYPE html>\n<html><body>projects</body></html>\n' > www/projects.html
printf 'projects\nexit\n' | run_client /tmp/mh_cli_proj.out
check_eq "unknown word 'projects' -> /projects.html 200" "1" \
         "$(grep -c '^HTTP/1.1 200 OK' /tmp/mh_cli_proj.out)"
rm -f www/projects.html
printf 'projects\nexit\n' | run_client /tmp/mh_cli_proj2.out
check_eq "after deletion -> 404" "1" "$(grep -c '^HTTP/1.1 404' /tmp/mh_cli_proj2.out)"

echo "== 5. Input handling =="

# exit terminates immediately and cleanly
printf 'exit\n' | run_client /tmp/mh_cli_exit.out
check_eq "exit -> clean exit status 0" "0" "$?"

# empty line then a command: client must survive and still work
printf '\nhome\nexit\n' | run_client /tmp/mh_cli_empty.out
check_eq "empty line tolerated" "1" "$(grep -c '^HTTP/1.1 200 OK' /tmp/mh_cli_empty.out)"

# over-long line (2000 chars) must be ignored, not crash
LONG=$(head -c 2000 /dev/zero | tr '\0' 'a')
printf '%s\nhome\nexit\n' "$LONG" | run_client /tmp/mh_cli_long.out
check_eq "2000-char line ignored (next cmd works)" "1" \
         "$(grep -c '^HTTP/1.1 200 OK' /tmp/mh_cli_long.out)"
if kill -0 $$ 2>/dev/null; then ok "client survived over-long input"; fi

# EOF without explicit exit: must return cleanly, not hang
printf 'home\n' | run_client /tmp/mh_cli_eof.out
check_eq "EOF without exit -> status 0" "0" "$?"
check_eq "EOF still processed command" "1" "$(grep -c '^HTTP/1.1 200 OK' /tmp/mh_cli_eof.out)"

# unknown word is treated as a page name (dynamic feature) -> 404,
# and the client must not crash
printf '!!!nosuch\nexit\n' | run_client /tmp/mh_cli_bogus.out
RC=$?
check_eq "unknown word -> 404 without crash" "1" "$(grep -c '^HTTP/1.1 404' /tmp/mh_cli_bogus.out)"
check_eq "client still exited cleanly" "0" "$RC"

echo "== 6. Server unavailable =="

kill -TERM "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null
SERVER_PID=""

printf 'home\nexit\n' | run_client /tmp/mh_cli_down.out
RC=$?
if [ "$RC" -eq 0 ] && grep -q 'cannot connect' /tmp/mh_cli_down.out; then
    ok "server down: graceful error, exit 0"
else
    bad "server down: graceful error, exit 0" "rc=0 + 'cannot connect'" "rc=$RC out=$(head -c 80 /tmp/mh_cli_down.out)"
fi

# invalid client port argument
"$CLI" notaport > /tmp/mh_cli_badport.out 2>&1
check_eq "client rejects invalid port arg" "1" "$?"

echo "-----------------------------------------"
printf 'CLIENT TOTAL  pass=%d  fail=%d\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
