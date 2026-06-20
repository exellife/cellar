#!/usr/bin/env bash
# `cellar provision <host>` scaffolds a working app bundle: it creates the bundle
# dir + data.db (identity schema) + hooks.lua, seeds an admin, and the provisioned
# app then serves — the seeded admin logs in over HTTP. provision_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: provision_test.sh <cellar-binary>}"
APPS="$(mktemp -d)"
trap 'kill "${SRV:-0}" 2>/dev/null || true; rm -rf "$APPS"' EXIT

fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok    $1 ($2)"; else echo "  FAIL  $1: got '$2' want '$3'"; fail=1; fi; }

# ---- provision ----
CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" provision shop.example admin@shop secret123 >/dev/null 2>&1
chk "data.db created"  "$([ -f "$APPS/shop.example/data.db" ] && echo yes || echo no)" "yes"
chk "hooks.lua created" "$([ -f "$APPS/shop.example/hooks.lua" ] && echo yes || echo no)" "yes"
chk "admin identity seeded" \
    "$(sqlite3 "$APPS/shop.example/data.db" "SELECT count(*) FROM cel_identities WHERE provider_uid='admin@shop'")" "1"

# re-provision is idempotent: keeps the existing admin (no duplicate, no clobber)
CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" provision shop.example >/dev/null 2>&1
chk "re-provision keeps admin" \
    "$(sqlite3 "$APPS/shop.example/data.db" "SELECT count(*) FROM cel_identities WHERE provider_uid='admin@shop'")" "1"

# reject a path-traversal host
if CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" provision '../evil' a@b cccccccc >/dev/null 2>&1; then
    chk "rejects bad host" "accepted" "rejected"
else
    chk "rejects bad host" "rejected" "rejected"
fi

# ---- the provisioned app serves: boot + login over HTTP ----
PORT=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")
CEL_PORT=$PORT CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error CEL_AUTH_RATELIMIT=0 "$BIN" >/dev/null 2>&1 &
SRV=$!
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done

TOKEN=$(curl -s -X POST -H 'Host: shop.example' -H 'Content-Type: application/json' \
        -d '{"email":"admin@shop","password":"secret123"}' "http://127.0.0.1:$PORT/auth/login" \
        | python3 -c "import sys,json;print(json.load(sys.stdin).get('token',''))")
chk "seeded admin logs in"  "$([ ${#TOKEN} -eq 64 ] && echo yes || echo no)" "yes"

[ "$fail" = 0 ] && echo "PROVISION PASS" || { echo "PROVISION FAIL"; exit 1; }
