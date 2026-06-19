#!/usr/bin/env bash
# RPC (#54) end-to-end: a throwaway DB with rpc_add(a,b) and a policy that
# whitelists it for admin; the server auto-migrates + seeds users on boot, then
# rpc_call.py drives /rpc over REST. rpc_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: rpc_test.sh <cellar-binary>}"
H="${CEL_DB_HOST:-localhost}"
U="${CEL_DB_USER:-postgres}"
DB=cel_rpc_test
PSQL="psql -h $H -U $U"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
POL=/tmp/cel_rpc_pol.json

cleanup() {
    $PSQL -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1 || true
    rm -f "$POL"
}
trap cleanup EXIT
cleanup
$PSQL -c "CREATE DATABASE $DB" >/dev/null
$PSQL -d "$DB" -c \
  "CREATE FUNCTION rpc_add(a int, b int) RETURNS int LANGUAGE sql AS \$\$ SELECT a + b \$\$;" >/dev/null

cat > "$POL" <<'JSON'
{
  "_roles": { "admin": { "superuser": true }, "editor": { "allow": ["list", "get"] } },
  "_rpc":   { "rpc_add": { "roles": ["admin"] } }
}
JSON

export CEL_DB_HOST="$H" CEL_DB_USER="$U" CEL_DB_NAME="$DB" CEL_LOG_LEVEL=warn
export CEL_POLICY_FILE="$POL"
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/rpc_call.py"
