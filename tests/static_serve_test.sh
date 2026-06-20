#!/usr/bin/env bash
# Per-bundle static serving: a provisioned app serves its public/ front-end
# (index at /, assets, SPA fallback for client-side routes), without shadowing the
# /auth /api /rpc routes, and with path-traversal blocked. static_serve_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: static_serve_test.sh <cellar-binary>}"
APPS="$(mktemp -d)"
trap 'kill "${SRV:-0}" 2>/dev/null || true; rm -rf "$APPS"' EXIT

fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok    $1 ($2)"; else echo "  FAIL  $1: got '$2' want '$3'"; fail=1; fi; }
code() { curl -s -o /dev/null -w '%{http_code}' -H 'Host: shop.example' "$@"; }
body() { curl -s -H 'Host: shop.example' "$@"; }

CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" provision shop.example admin@shop secret123 >/dev/null 2>&1
PUB="$APPS/shop.example/public"
printf 'SHOP-HOME' > "$PUB/index.html"
printf 'body{color:red}' > "$PUB/style.css"
mkdir -p "$PUB/assets"; printf 'ASSET-OK' > "$PUB/assets/data.txt"

PORT=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")
CEL_PORT=$PORT CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error CEL_AUTH_RATELIMIT=0 "$BIN" >/dev/null 2>&1 &
SRV=$!
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done
U="http://127.0.0.1:$PORT"

chk "GET / serves the app index"        "$(body "$U/")"                  "SHOP-HOME"
chk "GET /style.css serves the asset"   "$(body "$U/style.css")"         "body{color:red}"
chk "GET nested asset"                  "$(body "$U/assets/data.txt")"   "ASSET-OK"
chk "SPA fallback for /admin"           "$(body "$U/admin")"             "SHOP-HOME"
chk "SPA fallback for /profile/42"      "$(body "$U/profile/42")"        "SHOP-HOME"

# API routes are NOT shadowed by the SPA fallback
chk "POST /auth/login still works"      "$(code -X POST -H 'Content-Type: application/json' \
                                            -d '{"email":"admin@shop","password":"secret123"}' "$U/auth/login")" "200"
chk "GET /api/* not served as SPA"      "$(code "$U/api/products")"      "401"
chk "GET /health still works"           "$(code "$U/health")"            "200"

# path traversal out of public/ is blocked (not 200)
TRAV="$(code --path-as-is "$U/../../../../etc/passwd")"
chk "traversal blocked (not 200)"       "$([ "$TRAV" != "200" ] && echo blocked || echo LEAK)" "blocked"

# an app WITHOUT a public/ dir falls back (no public -> not the app index)
CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" provision bare.example admin@bare secret123 >/dev/null 2>&1
rm -rf "$APPS/bare.example/public"
chk "no public/ -> not the shop index" "$(curl -s -H 'Host: bare.example' "$U/" | grep -c 'SHOP-HOME' || true)" "0"

[ "$fail" = 0 ] && echo "STATIC SERVE PASS" || { echo "STATIC SERVE FAIL"; exit 1; }
