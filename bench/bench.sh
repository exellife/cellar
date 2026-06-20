#!/usr/bin/env bash
# cellar load/perf harness (SQLite). Boots a single-app server with a seeded demo
# catalog, then benchmarks the key paths (read / write / login) and the session-
# cache effect on authed reads.
#   bench/bench.sh <cellar-binary> [duration-seconds] [connections]
#   BENCH_PRODUCTS=20000 bench/bench.sh ...   # rows to seed (default 5000)
set -euo pipefail

BIN="${1:?usage: bench.sh <cellar-binary> [duration] [connections]}"
DUR="${2:-10}"; CONN="${3:-50}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
LT="$DIR/bench/loadtest.py"
NPROD="${BENCH_PRODUCTS:-5000}"

TMP="$(mktemp -d)"; DB="$TMP/data.db"

# Seed a demo catalog directly into a per-app SQLite file (uuid PKs auto-generate,
# exactly like a provisioned cellar app). No Postgres, no migrate.
DB="$DB" NPROD="$NPROD" python3 - <<'PY'
import os, sqlite3
db, n = os.environ['DB'], int(os.environ['NPROD'])
U = ("lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-4'||"
     "substr(lower(hex(randomblob(2))),2)||'-'||substr('89ab',abs(random())%4+1,1)||"
     "substr(lower(hex(randomblob(2))),2)||'-'||lower(hex(randomblob(6)))")
c = sqlite3.connect(db)
c.executescript(f"""
  CREATE TABLE categories(id UUID PRIMARY KEY DEFAULT ({U}), name TEXT NOT NULL);
  CREATE TABLE products(id UUID PRIMARY KEY DEFAULT ({U}), name TEXT NOT NULL,
    sku TEXT UNIQUE, price NUMERIC, in_stock INTEGER, category_id UUID);
  INSERT INTO categories(name) VALUES('A'),('B');
""")
c.executemany("INSERT INTO products(name, sku, price, in_stock) VALUES (?,?,?,?)",
              [(f"Product {i}", f"SKU-{i}", (i % 100) + 0.99, i % 500) for i in range(n)])
c.commit(); c.close()
PY

PORT=""; SRV=""
boot() {  # boot [EXTRA_ENV=val ...]
  PORT=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")
  env CEL_PORT=$PORT CEL_DATA_DB="$DB" CEL_LOG_LEVEL=error \
      CEL_AUTH_RATELIMIT=0 CEL_API_RATELIMIT=0 \
      CEL_SEED_USERS="bench@cellar.dev:benchpw:admin" \
      "$@" "$BIN" >/tmp/bench_$$.log 2>&1 &
  SRV=$!
  for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done
}
teardown() { kill "$SRV" 2>/dev/null || true; wait "$SRV" 2>/dev/null || true; }
trap 'teardown; rm -rf "$TMP" /tmp/bench_$$.log' EXIT

token() {
  curl -s -X POST -H 'Content-Type: application/json' \
       -d '{"email":"bench@cellar.dev","password":"benchpw"}' \
       "http://127.0.0.1:$PORT/auth/login" | python3 -c "import sys,json;print(json.load(sys.stdin).get('token',''))"
}

echo "== cellar load/perf  (duration=${DUR}s, connections=${CONN}, ${NPROD} products, rate-limit off) =="
echo "   model: $(uname -m), $(nproc) cores  |  SQLite per-app, plain HTTP"

# ---- session cache OFF (every authed request does a cel_sessions lookup) ----
boot
BASE="http://127.0.0.1:$PORT"
TOK="$(token)"
PID="$(curl -s -H "Authorization: Bearer $TOK" "$BASE/api/products?limit=1" \
       | python3 -c "import sys,json;r=json.load(sys.stdin).get('rows') or [{}];print(r[0].get('id',''))")"

echo
echo "[session cache OFF]"
python3 "$LT" --label "health (no auth, no DB)"  --url "$BASE/health" --connections "$CONN" --duration "$DUR"
python3 "$LT" --label "list products (authed)"   --url "$BASE/api/products?limit=20" -H "Authorization: Bearer $TOK" --connections "$CONN" --duration "$DUR"
[ -n "$PID" ] && python3 "$LT" --label "get product (authed)" --url "$BASE/api/products/$PID" -H "Authorization: Bearer $TOK" --connections "$CONN" --duration "$DUR"
# write path: per-request transaction + per-app write lock (sku omitted -> NULL, no unique clash)
python3 "$LT" --label "create product (write)"   --url "$BASE/api/products" --method POST \
        --body '{"name":"benchwrite","price":1}' -H "Authorization: Bearer $TOK" --connections "$CONN" --duration "$DUR"
python3 "$LT" --label "login (Argon2id)"         --url "$BASE/auth/login" --method POST \
        --body '{"email":"bench@cellar.dev","password":"benchpw"}' --connections 8 --duration "$DUR"
teardown

# ---- session cache ON: re-measure the authed read (skips the per-request lookup) ----
boot CEL_SESSION_CACHE_TTL=30
BASE="http://127.0.0.1:$PORT"
TOK="$(token)"
echo
echo "[session cache ON, TTL=30]"
python3 "$LT" --label "list products (cached auth)" --url "$BASE/api/products?limit=20" -H "Authorization: Bearer $TOK" --connections "$CONN" --duration "$DUR"
teardown

echo
echo "done. (client-observed throughput/latency from this stdlib generator; a native"
echo " tool like wrk pushes higher if the generator itself becomes the bottleneck.)"
