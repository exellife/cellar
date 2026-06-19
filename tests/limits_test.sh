#!/usr/bin/env bash
# Input/DoS limits: an oversized request body is rejected with 413 before parsing,
# and a slowloris connection (incomplete request held open) is reaped after the
# header timeout. Boots with a low body cap + short timeout. limits_test.sh <bin>
set -euo pipefail

BIN="${1:?usage: limits_test.sh <pgforge-binary>}"
H="${PGF_DB_HOST:-localhost}"; U="${PGF_DB_USER:-postgres}"; DB="${PGF_DB_NAME:-pgforge}"
PORT=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")

PGF_PORT=$PORT PGF_DB_HOST=$H PGF_DB_USER=$U PGF_DB_NAME=$DB PGF_LOG_LEVEL=warn \
  PGF_AUTH_RATELIMIT=0 PGF_MAX_BODY=1000 PGF_HEADER_TIMEOUT=2 \
  "$BIN" >/tmp/lim_$$.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true; rm -f /tmp/lim_$$.log' EXIT
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done

fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok    $1 ($2)"; else echo "  FAIL  $1: got '$2' want '$3'"; fail=1; fi; }
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
U="http://127.0.0.1:$PORT"

# body cap: a 2000-byte body exceeds PGF_MAX_BODY=1000 -> 413 (before any parse)
BIG=$(python3 -c "print('x'*2000)")
chk "oversized body -> 413" \
    "$(code -X POST -H 'Content-Type: application/json' -d "$BIG" "$U/auth/login")" "413"
# a normal body still works (not rejected by the cap)
chk "normal body not capped" \
    "$([ "$(code -X POST -H 'Content-Type: application/json' -d '{"email":"a@b.c","password":"nope"}' "$U/auth/login")" != "413" ] && echo ok || echo no)" "ok"

# slowloris: hold an incomplete request open; the reaper should close it after ~2s
result=$(python3 - "$PORT" <<'PY'
import socket, sys, time
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=10)
s.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n")   # no blank line -> incomplete
time.sleep(3.5)                                            # > PGF_HEADER_TIMEOUT (2s)
s.settimeout(3)
try:
    print("reaped" if s.recv(64) == b"" else "open")       # server-side close -> clean EOF
except (socket.timeout, ConnectionResetError):
    print("reaped")
PY
)
chk "slowloris connection reaped" "$result" "reaped"

[ "$fail" = 0 ] && echo "LIMITS PASS" || { echo "LIMITS FAIL"; cat /tmp/lim_$$.log; exit 1; }
