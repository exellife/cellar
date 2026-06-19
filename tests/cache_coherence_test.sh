#!/usr/bin/env bash
# M-1/M-2: with the per-process session cache enabled, out-of-band revocation /
# role / tenant changes have up-to-TTL propagation latency. Approach A makes that
# bound VISIBLE: the server warns at startup when the cache is on, and the
# revoke-sessions CLI warns that running servers may still honor revoked tokens.
# cache_coherence_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: cache_coherence_test.sh <cellar-binary>}"
H="${CEL_DB_HOST:-localhost}"; U="${CEL_DB_USER:-postgres}"; DB="${CEL_DB_NAME:-cellar}"
LOG="/tmp/cachecoh_$$.log"; SRV=""
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null || true; rm -f "$LOG"; }
trap cleanup EXIT

fail=0
have()    { grep -q "$1" "$LOG"; }
chk()     { if [ "$2" = ok ]; then echo "  ok    $1"; else echo "  FAIL  $1"; fail=1; fi; }

boot() {  # boot <ttl>: capture the startup log, wait for listen, then stop
    local ttl="$1"
    local port; port=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")
    env CEL_PORT="$port" CEL_DB_HOST="$H" CEL_DB_USER="$U" CEL_DB_NAME="$DB" \
        CEL_LOG_LEVEL=warn CEL_SESSION_CACHE_TTL="$ttl" "$BIN" >"$LOG" 2>&1 &
    SRV=$!
    for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$port/health" && break; sleep 0.1; done
    sleep 0.2
    kill "$SRV" 2>/dev/null || true; wait "$SRV" 2>/dev/null || true; SRV=""
}

echo "== M-1/M-2 cache-coherence visibility =="

boot 120
chk "startup warns when cache enabled" "$(have 'out-of-band revocation' && echo ok || echo no)"

boot 0
chk "no warning when cache disabled"   "$(have 'out-of-band revocation' && echo no || echo ok)"

# the revoke-sessions CLI warns about cache latency when a TTL is configured
CEL_DB_HOST="$H" CEL_DB_USER="$U" CEL_DB_NAME="$DB" CEL_LOG_LEVEL=warn CEL_SESSION_CACHE_TTL=120 \
    "$BIN" revoke-sessions someone@example.invalid >"$LOG" 2>&1 || true
chk "revoke-sessions CLI warns about cache" "$(have 'may still honor these tokens' && echo ok || echo no)"

echo
[ $fail = 0 ] && echo PASS || { echo FAIL; cat "$LOG"; exit 1; }
