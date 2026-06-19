#!/usr/bin/env bash
# Pooled multi-tenancy (Model B) end-to-end: two tenants on ONE cellar, proving
# cross-tenant isolation — a tenant sees only its own rows, and CREATE forces the
# caller's tenant even if the client tries to spoof another. Usage:
#   tenancy_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: tenancy_test.sh <cellar-binary>}"
H="${CEL_DB_HOST:-localhost}"
U="${CEL_DB_USER:-postgres}"
DB="cel_tenancy_test"
PSQL="psql -h $H -U $U"

cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null || true
            $PSQL -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1 || true; }
trap cleanup EXIT

$PSQL -tc "SELECT 1 FROM pg_database WHERE datname='$DB'" | grep -q 1 \
  && $PSQL -c "DROP DATABASE $DB" >/dev/null
$PSQL -c "CREATE DATABASE $DB" >/dev/null

export CEL_DB_HOST="$H" CEL_DB_USER="$U" CEL_DB_NAME="$DB"

# 1. core + tenancy schema (cel_tenants, cel_users.tenant_id)
"$BIN" migrate --tenancy >/dev/null

# 2. two tenants + a tenant-scoped business table, populated for both tenants.
#    items carries tenant_id, so the engine auto-scopes it in pooled mode.
T1=$($PSQL -d "$DB" -tAc "INSERT INTO cel_tenants(name) VALUES('Acme')   RETURNING id" | head -n1 | tr -d '[:space:]')
T2=$($PSQL -d "$DB" -tAc "INSERT INTO cel_tenants(name) VALUES('Globex') RETURNING id" | head -n1 | tr -d '[:space:]')
$PSQL -d "$DB" >/dev/null <<SQL
CREATE TABLE items (
    id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    tenant_id UUID,
    name      TEXT NOT NULL
);
INSERT INTO items(tenant_id, name) VALUES
    ('$T1','acme-1'), ('$T1','acme-2'),
    ('$T2','globex-1');
SQL

# 3. boot in pooled mode; seed one user per tenant (tenant_id set below).
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
CEL_PORT="$PORT" CEL_TENANT_COLUMN=tenant_id CEL_LOG_LEVEL=warn \
  CEL_SEED_USERS="u1@x.io:pw1:editor;u2@x.io:pw2:editor" "$BIN" >/tmp/cel_tenancy_srv.log 2>&1 &
SRV=$!
for i in $(seq 1 80); do
  (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null && { exec 3>&-; break; }
  sleep 0.1
done

# 4. assign each seeded user to a tenant (identity resolves tenant_id per request).
$PSQL -d "$DB" -c "UPDATE cel_users SET tenant_id='$T1' WHERE email='u1@x.io'" >/dev/null
$PSQL -d "$DB" -c "UPDATE cel_users SET tenant_id='$T2' WHERE email='u2@x.io'" >/dev/null

# 5. REST assertions.
PORT="$PORT" T1="$T1" T2="$T2" python3 - <<'PY'
import http.client, json, os, sys
PORT=int(os.environ["PORT"]); T2=os.environ["T2"]
ok=0; fail=0
def check(name, cond, extra=""):
    global ok, fail
    print(f"  {'ok' if cond else 'FAIL':<5} {name} {extra}")
    ok+=bool(cond); fail+=(not cond)
def req(method, path, body=None, token=None):
    c=http.client.HTTPConnection("127.0.0.1", PORT, timeout=10); h={}
    if body is not None: h["Content-Type"]="application/json"
    if token: h["Authorization"]="Bearer "+token
    c.request(method, path, json.dumps(body) if body is not None else None, h)
    r=c.getresponse(); d=r.read(); c.close()
    try: return r.status, json.loads(d)
    except Exception: return r.status, None
def login(e,p):
    s,b=req("POST","/auth/login",{"email":e,"password":p}); return (b or {}).get("token")

t1=login("u1@x.io","pw1"); t2=login("u2@x.io","pw2")
check("both tenants log in", bool(t1) and bool(t2))

s,b=req("GET","/api/items",token=t1)
names1=sorted(row["name"] for row in (b or {}).get("rows",[]))
check("tenant1 sees only its rows", s==200 and names1==["acme-1","acme-2"], names1)

s,b=req("GET","/api/items",token=t2)
names2=sorted(row["name"] for row in (b or {}).get("rows",[]))
check("tenant2 sees only its rows", s==200 and names2==["globex-1"], names2)

# cross-tenant get: tenant1 asks for a tenant2 row by id -> not found (scoped out)
s,b=req("GET","/api/items",token=t2)
globex_id=(b or {}).get("rows",[{}])[0].get("id")
s,_=req("GET",f"/api/items/{globex_id}",token=t1)
check("cross-tenant get -> 404", s==404, f"got {s}")

# create as tenant1, trying to spoof tenant2 -> tenant forced to caller's (T1)
s,b=req("POST","/api/items",{"name":"new","tenant_id":T2},token=t1)
row=(b or {}).get("row",{})
check("create forces caller tenant", s in (200,201) and row.get("tenant_id")!=T2 and row.get("name")=="new",
      f"tenant_id={row.get('tenant_id')}")

print(f"\n== tenancy: {ok} ok, {fail} fail ==")
sys.exit(1 if fail else 0)
PY
