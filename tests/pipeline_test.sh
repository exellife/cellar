#!/usr/bin/env bash
# libpq pipelining (perf lever) end-to-end. In pooled mode the data-API path
# normally does BEGIN + set_config(app.tenant_id) + query + COMMIT as four round
# trips; with PGF_DB_PIPELINE=1 they go as ONE pipelined round trip. This proves
# two things at once:
#   (1) CORRECTNESS — the same cross-tenant isolation the sequential path
#       guarantees (a tenant sees only its rows; CREATE forces the caller's
#       tenant) still holds when the transaction is pipelined.
#   (2) ACTIVE — pgf_db_pipelined_txns_total actually advances with traffic.
#
# Negative control: re-run with PGF_DB_PIPELINE=0 (or unset) — the server takes
# the sequential path, the counter stays 0, and the "pipelining active" assertion
# below fails (so the test genuinely exercises the lever).
#
#   pipeline_test.sh <pgforge-binary>
set -euo pipefail

BIN="${1:?usage: pipeline_test.sh <pgforge-binary>}"
H="${PGF_DB_HOST:-localhost}"
U="${PGF_DB_USER:-postgres}"
DB="pgf_pipeline_test"
PSQL="psql -h $H -U $U"

cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null || true
            $PSQL -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1 || true; }
trap cleanup EXIT

$PSQL -tc "SELECT 1 FROM pg_database WHERE datname='$DB'" | grep -q 1 \
  && $PSQL -c "DROP DATABASE $DB" >/dev/null
$PSQL -c "CREATE DATABASE $DB" >/dev/null

export PGF_DB_HOST="$H" PGF_DB_USER="$U" PGF_DB_NAME="$DB"

"$BIN" migrate --tenancy >/dev/null

T1=$($PSQL -d "$DB" -tAc "INSERT INTO pgf_tenants(name) VALUES('Acme')   RETURNING id" | head -n1 | tr -d '[:space:]')
T2=$($PSQL -d "$DB" -tAc "INSERT INTO pgf_tenants(name) VALUES('Globex') RETURNING id" | head -n1 | tr -d '[:space:]')
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

PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
# PGF_DB_PIPELINE defaults to 1 here; override to 0 for the negative control.
PGF_PORT="$PORT" PGF_TENANT_COLUMN=tenant_id PGF_LOG_LEVEL=warn \
  PGF_DB_PIPELINE="${PGF_DB_PIPELINE:-1}" PGF_METRICS_TOKEN="pipe-scrape-secret" \
  PGF_SEED_USERS="u1@x.io:pw1:editor;u2@x.io:pw2:editor" "$BIN" >/tmp/pgf_pipeline_srv.log 2>&1 &
SRV=$!
for i in $(seq 1 80); do
  (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null && { exec 3>&-; break; }
  sleep 0.1
done

$PSQL -d "$DB" -c "UPDATE pgf_users SET tenant_id='$T1' WHERE email='u1@x.io'" >/dev/null
$PSQL -d "$DB" -c "UPDATE pgf_users SET tenant_id='$T2' WHERE email='u2@x.io'" >/dev/null

PORT="$PORT" T1="$T1" T2="$T2" python3 - <<'PY'
import http.client, json, os, sys
PORT=int(os.environ["PORT"]); T2=os.environ["T2"]
TOKEN="pipe-scrape-secret"
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
def pipelined():
    c=http.client.HTTPConnection("127.0.0.1", PORT, timeout=10)
    c.request("GET","/metrics",headers={"Authorization":"Bearer "+TOKEN})
    doc=c.getresponse().read().decode(); c.close()
    for line in doc.splitlines():
        if line.startswith("pgf_db_pipelined_txns_total") and not line.startswith("#"):
            return float(line.split()[-1])
    return 0.0

t1=login("u1@x.io","pw1"); t2=login("u2@x.io","pw2")
check("both tenants log in", bool(t1) and bool(t2))

base=pipelined()

# ---- isolation must hold identically under pipelining ----
s,b=req("GET","/api/items",token=t1)
names1=sorted(row["name"] for row in (b or {}).get("rows",[]))
check("tenant1 sees only its rows", s==200 and names1==["acme-1","acme-2"], names1)

s,b=req("GET","/api/items",token=t2)
names2=sorted(row["name"] for row in (b or {}).get("rows",[]))
check("tenant2 sees only its rows", s==200 and names2==["globex-1"], names2)

s,b=req("GET","/api/items",token=t2)
globex_id=(b or {}).get("rows",[{}])[0].get("id")
s,_=req("GET",f"/api/items/{globex_id}",token=t1)
check("cross-tenant get -> 404", s==404, f"got {s}")

s,b=req("POST","/api/items",{"name":"new","tenant_id":T2},token=t1)
row=(b or {}).get("row",{})
check("create forces caller tenant",
      s in (200,201) and row.get("tenant_id")!=T2 and row.get("name")=="new",
      f"tenant_id={row.get('tenant_id')}")

# drive a few more reads, then confirm the pipeline counter advanced
for _ in range(10):
    req("GET","/api/items",token=t1)
after=pipelined()
print(f"  .. pgf_db_pipelined_txns_total {base:.0f} -> {after:.0f}")
check("pipelining active (counter advanced)", after >= base + 10, f"+{after-base:.0f}")

print(f"\n== pipeline: {ok} ok, {fail} fail ==")
sys.exit(1 if fail else 0)
PY
