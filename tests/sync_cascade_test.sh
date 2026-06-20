#!/usr/bin/env bash
# sync cascade soft-delete: deleting a parent on a syncable table soft-deletes (and
# rev-stamps, so they propagate) the children a real ON DELETE CASCADE would remove —
# recursively — while leaving non-CASCADE FK children alone. Boots its own server.
#   sync_cascade_test.sh <cellar-binary>
set -uo pipefail

BIN="${1:?usage: sync_cascade_test.sh <cellar-binary>}"
APPS="$(mktemp -d)"; B="$APPS/localhost"
PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
SRV=""
trap 'kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; rm -rf "$APPS"' EXIT

ok=0; fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok   $1"; ok=$((ok+1)); else echo "  FAIL $1 (got '$2' want '$3')"; fail=$((fail+1)); fi; }
q() { sqlite3 "$B/data.db" "$1"; }

CEL_APPS_DIR="$APPS" "$BIN" provision localhost a@b.c pw345678 >/dev/null 2>&1
q "CREATE TABLE parent(id TEXT PRIMARY KEY, name TEXT, rev INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0);
   CREATE TABLE child_c(id TEXT PRIMARY KEY, parent_id TEXT, rev INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0, FOREIGN KEY(parent_id) REFERENCES parent(id) ON DELETE CASCADE);
   CREATE TABLE child_n(id TEXT PRIMARY KEY, parent_id TEXT, rev INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0, FOREIGN KEY(parent_id) REFERENCES parent(id));
   CREATE TABLE grand(id TEXT PRIMARY KEY, child_id TEXT, rev INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0, FOREIGN KEY(child_id) REFERENCES child_c(id) ON DELETE CASCADE);"

env CEL_PORT="$PORT" CEL_APPS_DIR="$APPS" CEL_AUTH_RATELIMIT=0 CEL_API_RATELIMIT=0 CEL_LOG_LEVEL=error "$BIN" >/tmp/scas_$$.log 2>&1 &
SRV=$!
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done
H="Host: localhost"
TOK="$(curl -s -H "$H" -X POST -H 'Content-Type: application/json' -d '{"email":"a@b.c","password":"pw345678"}' "http://127.0.0.1:$PORT/auth/login" | python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')"
A() { curl -s -H "$H" -H "Authorization: Bearer $TOK" "$@"; }

# parent p1; cascade child cc1; non-cascade child cn1; grandchild g1 (under cc1)
A -X POST -d '{"mutations":[
  {"op":"put","table":"parent","id":"p1","values":{"name":"P"}},
  {"op":"put","table":"child_c","id":"cc1","values":{"parent_id":"p1"}},
  {"op":"put","table":"child_n","id":"cn1","values":{"parent_id":"p1"}},
  {"op":"put","table":"grand","id":"g1","values":{"child_id":"cc1"}}
]}' "http://127.0.0.1:$PORT/sync/push" >/dev/null
CUR="$(A -X POST -d '{"since":0,"device_id":"d1"}' "http://127.0.0.1:$PORT/sync/pull" | python3 -c 'import sys,json;print(json.load(sys.stdin)["cursor"])')"

# delete the parent
A -X DELETE "http://127.0.0.1:$PORT/api/parent/p1" >/dev/null

chk "parent soft-deleted"                "$(q "SELECT deleted FROM parent WHERE id='p1';")"  "1"
chk "CASCADE child soft-deleted"         "$(q "SELECT deleted FROM child_c WHERE id='cc1';")" "1"
chk "grandchild soft-deleted (recursion)" "$(q "SELECT deleted FROM grand WHERE id='g1';")"   "1"
chk "non-CASCADE child left ALIVE"       "$(q "SELECT deleted FROM child_n WHERE id='cn1';")" "0"
chk "cascaded child got a fresh rev (> parent create rev)" "$(q "SELECT rev > 4 FROM child_c WHERE id='cc1';")" "1"

# the deletions must PROPAGATE: device d1 pulls since its cursor and sees the tombstones
TOMBS="$(A -X POST -d "{\"since\":$CUR,\"device_id\":\"d1\"}" "http://127.0.0.1:$PORT/sync/pull" | python3 -c '
import sys,json
ch=json.load(sys.stdin)["changes"]
n=sum(1 for t in ("parent","child_c","grand") for r in ch.get(t,[]) if r.get("deleted")==1)
print(n)')"
chk "parent+cascade+grand tombstones propagate on pull" "$TOMBS" "3"

rm -f /tmp/scas_$$.log
echo
if [ "$fail" -eq 0 ]; then echo "PASS  ($ok ok, 0 failed)"; exit 0; else echo "FAIL  ($ok ok, $fail failed)"; exit 1; fi
