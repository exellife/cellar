#!/usr/bin/env bash
# Postgres RLS defense-in-depth: prove the DATABASE confines a raw query to the
# caller's tenant, independent of pgforge's app-level scoping. We SET ROLE to a
# non-superuser (superusers bypass RLS), bind app.tenant_id, and run a plain
# `SELECT * FROM items` — RLS, not the app, does the filtering.
#   rls_test.sh <pgforge-binary>
set -euo pipefail

BIN="${1:?usage: rls_test.sh <pgforge-binary>}"
H="${PGF_DB_HOST:-localhost}"
U="${PGF_DB_USER:-postgres}"
DB="pgf_rls_test"
ROLE="pgf_app_rlstest"
PSQL="psql -h $H -U $U"

cleanup() {
    $PSQL -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1 || true
    $PSQL -c "DROP ROLE IF EXISTS $ROLE"   >/dev/null 2>&1 || true
}
trap cleanup EXIT
cleanup
$PSQL -c "CREATE DATABASE $DB" >/dev/null
$PSQL -c "CREATE ROLE $ROLE NOLOGIN" >/dev/null   # SET ROLE only; no login needed

export PGF_DB_HOST="$H" PGF_DB_USER="$U" PGF_DB_NAME="$DB" PGF_TENANT_COLUMN=tenant_id
"$BIN" migrate --tenancy >/dev/null

T1=$($PSQL -d "$DB" -tAc "INSERT INTO pgf_tenants(name) VALUES('A') RETURNING id" | head -n1 | tr -d '[:space:]')
T2=$($PSQL -d "$DB" -tAc "INSERT INTO pgf_tenants(name) VALUES('B') RETURNING id" | head -n1 | tr -d '[:space:]')
$PSQL -d "$DB" >/dev/null <<SQL
CREATE TABLE items (id UUID PRIMARY KEY DEFAULT gen_random_uuid(), tenant_id UUID, name TEXT);
INSERT INTO items(tenant_id,name) VALUES ('$T1','a1'),('$T1','a2'),('$T2','b1');
GRANT SELECT, INSERT, UPDATE, DELETE ON items TO $ROLE;
SQL

# enable RLS + the isolation policy on every tenant-scoped table
"$BIN" tenancy-protect >/dev/null

# Count rows a RAW query sees under each app.tenant_id, as the non-superuser role.
seen() { $PSQL -d "$DB" -tAc \
    "SET ROLE $ROLE; SELECT set_config('app.tenant_id','$1',false); SELECT count(*) FROM items" \
    | tail -n1 | tr -d '[:space:]'; }
# unset context (fresh connection, no set_config) -> fail-closed
none=$($PSQL -d "$DB" -tAc "SET ROLE $ROLE; SELECT count(*) FROM items" | tail -n1 | tr -d '[:space:]')

c1=$(seen "$T1"); c2=$(seen "$T2"); call=$(seen '*')

fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok    $1 ($2)"; else echo "  FAIL  $1: got '$2' want '$3'"; fail=1; fi; }
echo "Postgres RLS isolation (raw SELECT, non-superuser role):"
chk "tenant A sees only its 2 rows" "$c1"   "2"
chk "tenant B sees only its 1 row"  "$c2"   "1"
chk "platform '*' sees all 3 rows"  "$call" "3"
chk "no tenant context -> 0 (fail-closed)" "$none" "0"

[ "$fail" = 0 ] && echo "RLS PASS" || { echo "RLS FAIL"; exit 1; }
