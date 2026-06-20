#!/usr/bin/env bash
# sync_gc e2e: tombstone GC honors per-device cursors. Boots its own server (the GC
# is a CLI, so it needs the binary). sync_gc_test.sh <cellar-binary>
set -uo pipefail

BIN="${1:?usage: sync_gc_test.sh <cellar-binary>}"
APPS="$(mktemp -d)"; B="$APPS/localhost"
PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
SRV=""
trap 'kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; rm -rf "$APPS"' EXIT

ok=0; fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok   $1"; ok=$((ok+1)); else echo "  FAIL $1 (got '$2' want '$3')"; fail=$((fail+1)); fi; }
q() { sqlite3 "$B/data.db" "$1"; }

CEL_APPS_DIR="$APPS" "$BIN" provision localhost admin@s.local synctest123 >/dev/null 2>&1
q "CREATE TABLE items(id TEXT PRIMARY KEY, name TEXT NOT NULL, rev INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0);"
env CEL_PORT="$PORT" CEL_APPS_DIR="$APPS" CEL_AUTH_RATELIMIT=0 CEL_API_RATELIMIT=0 CEL_LOG_LEVEL=error "$BIN" >/tmp/sgc_$$.log 2>&1 &
SRV=$!
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done
H="Host: localhost"
TOK="$(curl -s -H "$H" -X POST -H 'Content-Type: application/json' -d '{"email":"admin@s.local","password":"synctest123"}' "http://127.0.0.1:$PORT/auth/login" | python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')"
A() { curl -s -H "$H" -H "Authorization: Bearer $TOK" "$@"; }

# a,b,c (rev 1-3); delete a (rev 4), b (rev 5) -> tombstones a@4, b@5
A -X POST -d '{"mutations":[{"op":"put","table":"items","id":"a","values":{"name":"a"}},{"op":"put","table":"items","id":"b","values":{"name":"b"}},{"op":"put","table":"items","id":"c","values":{"name":"c"}}]}' "http://127.0.0.1:$PORT/sync/push" >/dev/null
A -X DELETE "http://127.0.0.1:$PORT/api/items/a" >/dev/null
A -X DELETE "http://127.0.0.1:$PORT/api/items/b" >/dev/null
chk "two tombstones exist" "$(q "SELECT count(*) FROM items WHERE deleted=1;")" "2"

# GC with NO devices registered -> conservative, purges nothing
CEL_APPS_DIR="$APPS" "$BIN" sync-gc localhost >/dev/null 2>&1
chk "no devices -> tombstones kept" "$(q "SELECT count(*) FROM items WHERE deleted=1;")" "2"

# device dev-1 durably at since=4 -> min cursor 4 -> purge rev<=4 (a@4), keep b@5
A -X POST -d '{"since":4,"device_id":"dev-1"}' "http://127.0.0.1:$PORT/sync/pull" >/dev/null
chk "device cursor recorded" "$(q "SELECT cursor FROM _sync_devices WHERE device_id='dev-1';")" "4"
CEL_APPS_DIR="$APPS" "$BIN" sync-gc localhost >/dev/null 2>&1
chk "tombstone a@4 purged" "$(q "SELECT count(*) FROM items WHERE id='a';")" "0"
chk "tombstone b@5 kept (above min cursor)" "$(q "SELECT count(*) FROM items WHERE id='b' AND deleted=1;")" "1"
chk "live row c intact" "$(q "SELECT name FROM items WHERE id='c';")" "c"

rm -f /tmp/sgc_$$.log
echo
if [ "$fail" -eq 0 ]; then echo "PASS  ($ok ok, 0 failed)"; exit 0; else echo "FAIL  ($ok ok, $fail failed)"; exit 1; fi
