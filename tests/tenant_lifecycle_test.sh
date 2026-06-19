#!/usr/bin/env bash
# Tenant lifecycle + admin tiers (pooled mode):
#   - create-platform-admin / create-tenant provisioning CLIs
#   - a tenant-admin is confined to its own tenant; the platform-admin sees ALL
#   - suspend-tenant blocks the tenant's users from authenticating; resume restores
#   tenant_lifecycle_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: tenant_lifecycle_test.sh <cellar-binary>}"
H="${CEL_DB_HOST:-localhost}"
U="${CEL_DB_USER:-postgres}"
DB="cel_lifecycle_test"
PSQL="psql -h $H -U $U"

cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null || true
            $PSQL -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1 || true; }
trap cleanup EXIT
$PSQL -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1 || true
$PSQL -c "CREATE DATABASE $DB" >/dev/null

export CEL_DB_HOST="$H" CEL_DB_USER="$U" CEL_DB_NAME="$DB" CEL_TENANT_COLUMN=tenant_id CEL_LOG_LEVEL=warn

# provision via the CLIs (all pre-boot)
"$BIN" migrate --tenancy >/dev/null
"$BIN" create-platform-admin ops@pf.io pw-platform >/dev/null
"$BIN" create-tenant Acme   admin@acme.io   pw-acme   >/dev/null
"$BIN" create-tenant Globex admin@globex.io pw-globex >/dev/null

# a tenant-scoped business table, populated for both tenants
T1=$($PSQL -d "$DB" -tAc "SELECT id FROM cel_tenants WHERE name='Acme'"   | tr -d '[:space:]')
T2=$($PSQL -d "$DB" -tAc "SELECT id FROM cel_tenants WHERE name='Globex'" | tr -d '[:space:]')
$PSQL -d "$DB" >/dev/null <<SQL
CREATE TABLE items (id UUID PRIMARY KEY DEFAULT gen_random_uuid(), tenant_id UUID, name TEXT);
INSERT INTO items(tenant_id,name) VALUES ('$T1','acme-1'), ('$T1','acme-2'), ('$T2','globex-1');
SQL

PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
CEL_PORT="$PORT" "$BIN" >/tmp/cel_lifecycle_srv.log 2>&1 &
SRV=$!
for i in $(seq 1 80); do (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null && { exec 3>&-; break; }; sleep 0.1; done

PORT="$PORT" BIN="$BIN" python3 - <<'PY'
import http.client, json, os, subprocess, sys
PORT=int(os.environ["PORT"]); BIN=os.environ["BIN"]
ok=0; fail=0
def check(n,c,x=""):
    global ok,fail; print(f"  {'ok' if c else 'FAIL':<5} {n} {x}"); ok+=bool(c); fail+=(not c)
def req(method,path,body=None,token=None):
    cn=http.client.HTTPConnection("127.0.0.1",PORT,timeout=10); h={}
    if body is not None: h["Content-Type"]="application/json"
    if token: h["Authorization"]="Bearer "+token
    cn.request(method,path,json.dumps(body) if body is not None else None,h)
    r=cn.getresponse(); d=r.read(); cn.close()
    try: return r.status, json.loads(d)
    except Exception: return r.status, None
def login(e,p):
    s,b=req("POST","/auth/login",{"email":e,"password":p}); return s,(b or {}).get("token")

def names(tok):
    s,b=req("GET","/api/items",token=tok); return s, sorted(r["name"] for r in (b or {}).get("rows",[]))

_,acme = login("admin@acme.io","pw-acme")
_,glob = login("admin@globex.io","pw-globex")
_,plat = login("ops@pf.io","pw-platform")
check("all three admins log in", all([acme,glob,plat]))

s,n = names(acme);  check("tenant-admin (Acme) sees only Acme", s==200 and n==["acme-1","acme-2"], n)
s,n = names(glob);  check("tenant-admin (Globex) sees only Globex", s==200 and n==["globex-1"], n)
s,n = names(plat);  check("platform-admin sees ALL tenants", s==200 and n==["acme-1","acme-2","globex-1"], n)

# suspend Acme (out-of-band CLI) -> its admin can no longer authenticate
subprocess.run([BIN,"suspend-tenant","Acme"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
               env={**os.environ})
s,tok = login("admin@acme.io","pw-acme"); check("suspended tenant login -> denied", tok is None, f"status {s}")
# Globex unaffected
s,_ = login("admin@globex.io","pw-globex"); check("other tenant still works", s==200)
# resume -> back in
subprocess.run([BIN,"resume-tenant","Acme"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
               env={**os.environ})
s,tok = login("admin@acme.io","pw-acme"); check("resumed tenant login -> ok", tok is not None, f"status {s}")

print(f"\n== lifecycle: {ok} ok, {fail} fail ==")
sys.exit(1 if fail else 0)
PY
