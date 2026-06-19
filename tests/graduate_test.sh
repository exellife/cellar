#!/usr/bin/env bash
# Graduate a pooled tenant to its own standalone (Model A) deployment:
# `export-tenant` emits a loadable SQL script; loading it into a fresh
# single-tenant DB reproduces ONLY that tenant's data (users + business rows),
# with no trace of other tenants. graduate_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: graduate_test.sh <cellar-binary>}"
H="${CEL_DB_HOST:-localhost}"
U="${CEL_DB_USER:-postgres}"
SRC=cel_grad_src
DST=cel_grad_dst
PSQL="psql -h $H -U $U"
EXPORT=/tmp/cel_graduate_export.sql

cleanup() {
    $PSQL -c "DROP DATABASE IF EXISTS $SRC" >/dev/null 2>&1 || true
    $PSQL -c "DROP DATABASE IF EXISTS $DST" >/dev/null 2>&1 || true
    rm -f "$EXPORT"
}
trap cleanup EXIT
cleanup
$PSQL -c "CREATE DATABASE $SRC" >/dev/null
$PSQL -c "CREATE DATABASE $DST" >/dev/null

export CEL_DB_HOST="$H" CEL_DB_USER="$U" CEL_LOG_LEVEL=warn

# --- SOURCE: pooled deployment, two tenants + a tenant-scoped table ---
CEL_DB_NAME=$SRC CEL_TENANT_COLUMN=tenant_id "$BIN" migrate --tenancy >/dev/null
CEL_DB_NAME=$SRC CEL_TENANT_COLUMN=tenant_id "$BIN" create-tenant Acme   admin@acme.io   pw-acme   >/dev/null
CEL_DB_NAME=$SRC CEL_TENANT_COLUMN=tenant_id "$BIN" create-tenant Globex admin@globex.io pw-globex >/dev/null
T1=$($PSQL -d "$SRC" -tAc "SELECT id FROM cel_tenants WHERE name='Acme'"   | tr -d '[:space:]')
T2=$($PSQL -d "$SRC" -tAc "SELECT id FROM cel_tenants WHERE name='Globex'" | tr -d '[:space:]')
$PSQL -d "$SRC" >/dev/null <<SQL
CREATE TABLE items (id UUID PRIMARY KEY DEFAULT gen_random_uuid(), tenant_id UUID, name TEXT);
INSERT INTO items(tenant_id,name) VALUES ('$T1','acme-1'), ('$T1','acme-2'), ('$T2','globex-1');
SQL

# --- EXPORT Acme to a portable script ---
CEL_DB_NAME=$SRC CEL_TENANT_COLUMN=tenant_id "$BIN" export-tenant Acme > "$EXPORT"

# --- TARGET: a fresh standalone Model A deployment ---
CEL_DB_NAME=$DST "$BIN" migrate >/dev/null   # core only (no tenancy)
# recreate the business table WITHOUT the tenant column (single-tenant)
$PSQL -d "$DST" -c "CREATE TABLE items (id UUID PRIMARY KEY DEFAULT gen_random_uuid(), name TEXT)" >/dev/null
$PSQL -d "$DST" -f "$EXPORT" >/dev/null

# --- VERIFY the graduate ---
items=$($PSQL  -d "$DST" -tAc "SELECT string_agg(name, ',' ORDER BY name) FROM items")
admin=$($PSQL  -d "$DST" -tAc "SELECT count(*) FROM cel_users WHERE email='admin@acme.io'" | tr -d '[:space:]')
others=$($PSQL -d "$DST" -tAc "SELECT count(*) FROM items WHERE name LIKE 'globex%'" | tr -d '[:space:]')
notenant=$($PSQL -d "$DST" -tAc "SELECT count(*) FROM information_schema.columns WHERE table_name='cel_users' AND column_name='tenant_id'" | tr -d '[:space:]')
# credentials must travel too (cel_identities), else the graduated admin can't log in
cred=$($PSQL -d "$DST" -tAc "SELECT count(*) FROM cel_identities i JOIN cel_users u ON u.id=i.user_id WHERE u.email='admin@acme.io' AND i.provider='password'" | tr -d '[:space:]')

fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok    $1 ($2)"; else echo "  FAIL  $1: got '$2' want '$3'"; fail=1; fi; }
echo "Graduate Acme -> standalone Model A:"
chk "Acme's rows graduated"        "$items"    "acme-1,acme-2"
chk "Acme's admin graduated"       "$admin"    "1"
chk "admin's credential graduated" "$cred"     "1"
chk "no other tenant's data"       "$others"   "0"
chk "target is single-tenant"      "$notenant" "0"

[ "$fail" = 0 ] && echo "GRADUATE PASS" || { echo "GRADUATE FAIL"; exit 1; }
