#!/usr/bin/env bash
# Bundle export/import: a consistent online snapshot of an app (VACUUM INTO, while
# serving) packages data.db + hooks.lua + public/ into one archive; importing it
# under a DIFFERENT host reconstitutes a working app, with live sessions stripped.
# export_import_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: export_import_test.sh <cellar-binary>}"
APPS="$(mktemp -d)"; ARCHIVE="$(mktemp -u).tar.gz"
trap 'kill "${SRV:-0}" 2>/dev/null || true; rm -rf "$APPS" "$ARCHIVE"' EXIT

fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok    $1 ($2)"; else echo "  FAIL  $1: got '$2' want '$3'"; fail=1; fi; }

CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" provision shop.example admin@shop secret123 >/dev/null 2>&1
printf 'SHOPDATA' > "$APPS/shop.example/public/index.html"

# boot the source and create a live session (so we can prove sessions are stripped)
PORT=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")
CEL_PORT=$PORT CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error CEL_AUTH_RATELIMIT=0 "$BIN" >/dev/null 2>&1 &
SRV=$!
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done
curl -s -H 'Host: shop.example' -X POST -H 'Content-Type: application/json' \
     -d '{"email":"admin@shop","password":"secret123"}' "http://127.0.0.1:$PORT/auth/login" >/dev/null
chk "source has a live session" "$(sqlite3 "$APPS/shop.example/data.db" 'SELECT count(*) FROM cel_sessions')" "1"

# export WHILE the server is serving (online, consistent snapshot)
CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" export shop.example "$ARCHIVE" >/dev/null 2>&1
chk "archive created" "$([ -s "$ARCHIVE" ] && echo yes || echo no)" "yes"
kill "$SRV" 2>/dev/null; SRV=0

# import under a DIFFERENT host (rehost / clone)
CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" import clone.example "$ARCHIVE" >/dev/null 2>&1
chk "clone data.db present"   "$([ -f "$APPS/clone.example/data.db" ] && echo yes || echo no)" "yes"
chk "clone front-end travelled" "$(cat "$APPS/clone.example/public/index.html")" "SHOPDATA"
chk "clone hooks.lua travelled" "$([ -f "$APPS/clone.example/hooks.lua" ] && echo yes || echo no)" "yes"
chk "clone sessions stripped"  "$(sqlite3 "$APPS/clone.example/data.db" 'SELECT count(*) FROM cel_sessions')" "0"

# the imported app serves and the admin identity travelled (login works)
CEL_PORT=$PORT CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error CEL_AUTH_RATELIMIT=0 "$BIN" >/dev/null 2>&1 &
SRV=$!
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done
TOKEN=$(curl -s -H 'Host: clone.example' -X POST -H 'Content-Type: application/json' \
        -d '{"email":"admin@shop","password":"secret123"}' "http://127.0.0.1:$PORT/auth/login" \
        | python3 -c "import sys,json;print(json.load(sys.stdin).get('token',''))")
chk "admin identity travelled (login on clone)" "$([ ${#TOKEN} -eq 64 ] && echo yes || echo no)" "yes"

# import refuses to overwrite an existing app
if CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" import clone.example "$ARCHIVE" >/dev/null 2>&1; then
    chk "import refuses to clobber" "clobbered" "refused"
else
    chk "import refuses to clobber" "refused" "refused"
fi

[ "$fail" = 0 ] && echo "EXPORT/IMPORT PASS" || { echo "EXPORT/IMPORT FAIL"; exit 1; }
