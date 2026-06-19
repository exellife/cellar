#!/usr/bin/env bash
# H-4: a whitelisted RPC function defined SECURITY DEFINER bypasses row-level
# security (a tenant-isolation risk in pooled mode). cellar must warn loudly
# about it at startup, and must NOT warn about a normal SECURITY INVOKER function.
# rpc_secdef_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: rpc_secdef_test.sh <cellar-binary>}"
H="${CEL_DB_HOST:-localhost}"
U="${CEL_DB_USER:-postgres}"
DB=cel_rpc_secdef_test
PSQL="psql -h $H -U $U"
POL=/tmp/cel_rpc_secdef_pol.json
LOG="/tmp/cel_rpc_secdef_$$.log"
SRV=""

cleanup() {
    [ -n "$SRV" ] && kill "$SRV" 2>/dev/null || true
    [ -n "$SRV" ] && wait "$SRV" 2>/dev/null || true
    $PSQL -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1 || true
    rm -f "$POL" "$LOG"
}
trap cleanup EXIT
cleanup
$PSQL -c "CREATE DATABASE $DB" >/dev/null
$PSQL -d "$DB" -c \
  "CREATE FUNCTION rpc_safe(a int) RETURNS int LANGUAGE sql AS \$\$ SELECT a \$\$;" >/dev/null
$PSQL -d "$DB" -c \
  "CREATE FUNCTION rpc_danger(a int) RETURNS int LANGUAGE sql SECURITY DEFINER AS \$\$ SELECT a \$\$;" >/dev/null

cat > "$POL" <<'JSON'
{
  "_default": "deny",
  "_roles": { "admin": { "superuser": true } },
  "_rpc": { "rpc_safe": { "roles": ["admin"] }, "rpc_danger": { "roles": ["admin"] } }
}
JSON

PORT=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")
env CEL_PORT="$PORT" CEL_DB_HOST="$H" CEL_DB_USER="$U" CEL_DB_NAME="$DB" CEL_LOG_LEVEL=warn \
    CEL_TENANT_COLUMN=tenant_id CEL_POLICY_FILE="$POL" \
    "$BIN" >"$LOG" 2>&1 &
SRV=$!
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done
sleep 0.3   # let the startup audit flush to the log

echo "== H-4 SECURITY DEFINER RPC audit =="
pass=0
chk() { if eval "$2"; then echo "  ok    $1"; else echo "  FAIL  $1"; pass=1; fi; }
chk "SECURITY DEFINER fn warned"         "grep -q 'rpc_danger.*SECURITY DEFINER' '$LOG'"
chk "pooled-mode TENANT note present"    "grep -q 'rpc_danger.*TENANT' '$LOG'"
chk "SECURITY INVOKER fn not warned"     "! grep -q 'rpc_safe.*SECURITY DEFINER' '$LOG'"

echo
[ $pass -eq 0 ] && echo PASS || { echo FAIL; echo '--- server log ---'; cat "$LOG"; }
exit $pass
