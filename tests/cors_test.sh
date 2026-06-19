#!/usr/bin/env bash
# CORS end-to-end: boot with PGF_CORS_ORIGINS set and assert the response headers
# (allowed origin echoed, others denied, no Origin => none) and that an OPTIONS
# preflight returns 204 with the CORS headers. cors_test.sh <pgforge-binary>
set -euo pipefail

BIN="${1:?usage: cors_test.sh <pgforge-binary>}"
H="${PGF_DB_HOST:-localhost}"; U="${PGF_DB_USER:-postgres}"; DB="${PGF_DB_NAME:-pgforge}"
PORT=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")
ORIGIN="https://app.test"

PGF_PORT=$PORT PGF_DB_HOST=$H PGF_DB_USER=$U PGF_DB_NAME=$DB PGF_LOG_LEVEL=warn \
  PGF_CORS_ORIGINS="$ORIGIN, https://admin.test" \
  "$BIN" >/tmp/cors_$$.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true; rm -f /tmp/cors_$$.log' EXIT
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done

fail=0
chk() { if [ "$1" = 1 ]; then echo "  ok    $2"; else echo "  FAIL  $2"; fail=1; fi; }
has() { grep -qi "$1" <<<"$2" && echo 1 || echo 0; }
U="http://127.0.0.1:$PORT"

# 1. an allowed origin is echoed back
H1=$(curl -s -i -H "Origin: $ORIGIN" "$U/health")
chk "$(has "^Access-Control-Allow-Origin: $ORIGIN" "$H1")" "allowed origin echoed in ACAO"
chk "$(has "^Vary: Origin" "$H1")"                          "Vary: Origin present"

# 2. a non-allowed origin gets no CORS header
H2=$(curl -s -i -H "Origin: https://evil.test" "$U/health")
chk "$([ "$(has 'Access-Control-Allow-Origin' "$H2")" = 0 ] && echo 1 || echo 0)" "disallowed origin -> no ACAO"

# 3. a same-origin request (no Origin header) gets no CORS header
H3=$(curl -s -i "$U/health")
chk "$([ "$(has 'Access-Control-Allow-Origin' "$H3")" = 0 ] && echo 1 || echo 0)" "no Origin -> no ACAO"

# 4. an OPTIONS preflight is answered 204 with the CORS headers
H4=$(curl -s -i -X OPTIONS -H "Origin: $ORIGIN" -H "Access-Control-Request-Method: POST" "$U/api/products")
chk "$(has "^HTTP/1.1 204" "$H4")"                            "preflight -> 204"
chk "$(has "^Access-Control-Allow-Origin: $ORIGIN" "$H4")"    "preflight echoes origin"
chk "$(has "^Access-Control-Allow-Methods:.*POST" "$H4")"     "preflight allow-methods"
chk "$(has "^Access-Control-Allow-Headers:.*Authorization" "$H4")" "preflight allow-headers"
chk "$(has "^Access-Control-Max-Age:" "$H4")"                 "preflight max-age"

[ "$fail" = 0 ] && echo "CORS PASS" || { echo "CORS FAIL"; exit 1; }
