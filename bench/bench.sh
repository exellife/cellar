#!/usr/bin/env bash
# pgforge load/perf harness. Boots the server (rate limiter off, demo data),
# then benchmarks the key paths and shows the session-cache effect on authed reads.
#   bench/bench.sh <pgforge-binary> [duration-seconds] [connections]
set -euo pipefail

BIN="${1:?usage: bench.sh <pgforge-binary> [duration] [connections]}"
DUR="${2:-10}"; CONN="${3:-50}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
LT="$DIR/bench/loadtest.py"
H="${PGF_DB_HOST:-localhost}"; U="${PGF_DB_USER:-postgres}"; DB="${PGF_DB_NAME:-pgforge}"

# Make sure the demo tables (products/categories) exist to read against.
PGF_DB_HOST=$H PGF_DB_USER=$U PGF_DB_NAME=$DB "$BIN" migrate --demo >/dev/null 2>&1 || true

PORT=""; SRV=""
boot() {  # boot [EXTRA_ENV=val ...]
  PORT=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")
  env PGF_PORT=$PORT PGF_DB_HOST=$H PGF_DB_USER=$U PGF_DB_NAME=$DB \
      PGF_LOG_LEVEL=error PGF_AUTH_RATELIMIT=0 \
      PGF_SEED_USERS="bench@pgforge.dev:benchpw:admin" \
      "$@" "$BIN" >/tmp/bench_$$.log 2>&1 &
  SRV=$!
  for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done
}
teardown() { kill "$SRV" 2>/dev/null || true; wait "$SRV" 2>/dev/null || true; }
trap 'teardown; rm -f /tmp/bench_$$.log' EXIT

token() {
  curl -s -X POST -H 'Content-Type: application/json' \
       -d '{"email":"bench@pgforge.dev","password":"benchpw"}' \
       "http://127.0.0.1:$PORT/auth/login" | python3 -c "import sys,json;print(json.load(sys.stdin).get('token',''))"
}

echo "== pgforge load/perf  (duration=${DUR}s, connections=${CONN}, rate-limit off) =="
echo "   model: $(uname -m), $(nproc) cores"

# ---- session cache OFF (every authed request does a DB session lookup) ----
boot
BASE="http://127.0.0.1:$PORT"
TOK="$(token)"
PID="$(curl -s -H "Authorization: Bearer $TOK" "$BASE/api/products?limit=1" \
       | python3 -c "import sys,json;r=json.load(sys.stdin).get('rows') or [{}];print(r[0].get('id',''))")"

echo
echo "[session cache OFF]"
python3 "$LT" --label "health (no auth, no DB)"  --url "$BASE/health" --connections "$CONN" --duration "$DUR"
python3 "$LT" --label "list products (authed)"   --url "$BASE/api/products?limit=20" -H "Authorization: Bearer $TOK" --connections "$CONN" --duration "$DUR"
[ -n "$PID" ] && python3 "$LT" --label "get product (authed)" --url "$BASE/api/products/$PID" -H "Authorization: Bearer $TOK" --connections "$CONN" --duration "$DUR"
python3 "$LT" --label "login (Argon2id)"         --url "$BASE/auth/login" --method POST \
        --body '{"email":"bench@pgforge.dev","password":"benchpw"}' --connections 8 --duration "$DUR"
teardown

# ---- session cache ON: re-measure the authed read (skips the per-request DB lookup) ----
boot PGF_SESSION_CACHE_TTL=30
BASE="http://127.0.0.1:$PORT"
TOK="$(token)"
echo
echo "[session cache ON, TTL=30]"
python3 "$LT" --label "list products (cached auth)" --url "$BASE/api/products?limit=20" -H "Authorization: Bearer $TOK" --connections "$CONN" --duration "$DUR"
teardown

echo
echo "done. (numbers are client-observed throughput/latency from this stdlib generator;"
echo " a native tool like wrk can push higher request rates if the generator is the bottleneck.)"
