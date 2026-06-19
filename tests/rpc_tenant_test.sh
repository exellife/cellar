#!/usr/bin/env bash
# M-3 regression: in POOLED mode (CEL_TENANT_COLUMN set), an RPC whitelisted for
# 'anon' must still be DENIED for an unauthenticated (tenant-less) caller — it
# would otherwise execute with an empty app.tenant_id in an undefined RLS context.
# (In single-tenant mode the same anon RPC is allowed; the fix is pooled-mode only.)
# rpc_tenant_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: rpc_tenant_test.sh <cellar-binary>}"
H="${CEL_DB_HOST:-localhost}"
U="${CEL_DB_USER:-postgres}"
DB=cel_rpc_tenant_test
PSQL="psql -h $H -U $U"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
POL=/tmp/cel_rpc_tenant_pol.json

cleanup() {
    $PSQL -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1 || true
    rm -f "$POL"
}
trap cleanup EXIT
cleanup
$PSQL -c "CREATE DATABASE $DB" >/dev/null
$PSQL -d "$DB" -c \
  "CREATE FUNCTION rpc_add(a int, b int) RETURNS int LANGUAGE sql AS \$\$ SELECT a + b \$\$;" >/dev/null

# anon IS whitelisted for rpc_add — the pooled-mode empty-tenant seatbelt, not the
# whitelist, is what must deny the anonymous caller.
cat > "$POL" <<'JSON'
{
  "_default": "deny",
  "_roles": { "admin": { "superuser": true } },
  "_rpc":   { "rpc_add": { "roles": ["anon", "admin"] } }
}
JSON

export CEL_DB_HOST="$H" CEL_DB_USER="$U" CEL_DB_NAME="$DB" CEL_LOG_LEVEL=warn
export CEL_POLICY_FILE="$POL" CEL_TENANT_COLUMN=tenant_id
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/rpc_tenant.py"
