#!/usr/bin/env bash
# Write-scaling across apps: cellar serializes writes PER app (one writer per
# SQLite file) but different apps never block each other. This drives a FIXED total
# write load split across N ∈ {1,2,4,8} apps and reports aggregate writes/s — it
# should climb as the same load spreads over more files (until CPU/IO saturates).
#   bench/multiapp_write.sh <cellar-binary> [duration] [total-connections] ["N-list"]
set -euo pipefail

BIN="${1:?usage: multiapp_write.sh <cellar-binary> [duration] [total-conns] [\"1 2 4 8\"]}"
DUR="${2:-8}"; TOTAL="${3:-48}"; NLIST="${4:-1 2 4 8}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"; LT="$DIR/bench/loadtest.py"
MAXN=$(echo "$NLIST" | tr ' ' '\n' | sort -n | tail -1)

APPS="$(mktemp -d)"
PORT=""; SRV=""
teardown() { kill "$SRV" 2>/dev/null || true; wait "$SRV" 2>/dev/null || true; rm -rf "$APPS" /tmp/maw_$$_*; }
trap teardown EXIT

echo "== multi-app write scaling  (dur=${DUR}s, total-conns=${TOTAL}, apps: $NLIST) =="
echo "   model: $(uname -m), $(nproc) cores  |  one writer per app (SQLite file)"

# provision MAXN apps, each with a products table to write into
for i in $(seq 1 "$MAXN"); do
  CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" provision "a$i.local" "admin@a$i.local" benchpw >/dev/null 2>&1
  python3 -c "import sqlite3;c=sqlite3.connect('$APPS/a$i.local/data.db');c.execute('CREATE TABLE products(id INTEGER PRIMARY KEY, name TEXT NOT NULL, price NUMERIC)');c.commit();c.close()"
done

PORT=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")
env CEL_PORT=$PORT CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error CEL_AUTH_RATELIMIT=0 CEL_API_RATELIMIT=0 \
    "$BIN" >/tmp/maw_$$_srv.log 2>&1 &
SRV=$!
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done
BASE="http://127.0.0.1:$PORT"

# one admin token per app
declare -A TOK
for i in $(seq 1 "$MAXN"); do
  TOK[$i]=$(curl -s -H "Host: a$i.local" -X POST -H 'Content-Type: application/json' \
    -d "{\"email\":\"admin@a$i.local\",\"password\":\"benchpw\"}" "$BASE/auth/login" \
    | python3 -c "import sys,json;print(json.load(sys.stdin).get('token',''))")
done

echo
printf "  %-9s %-12s %-14s %-12s\n" "apps" "conns/app" "aggregate" "per-app avg"
for N in $NLIST; do
  PER=$(( TOTAL / N )); [ "$PER" -lt 1 ] && PER=1
  pids=(); outs=()
  for i in $(seq 1 "$N"); do
    OUT="/tmp/maw_$$_a$i"
    python3 "$LT" --label "a$i" --url "$BASE/api/products" --method POST --body '{"name":"w","price":1}' \
      -H "Host: a$i.local" -H "Authorization: Bearer ${TOK[$i]}" \
      --connections "$PER" --duration "$DUR" >"$OUT" 2>&1 &
    pids+=($!); outs+=("$OUT")
  done
  wait "${pids[@]}"
  agg=0; errs=0
  for o in "${outs[@]}"; do
    r=$(grep -oE '[0-9]+ req/s' "$o" | grep -oE '^[0-9]+'); agg=$(( agg + ${r:-0} ))
    e=$(grep -oE 'errors=[0-9]+' "$o" | grep -oE '[0-9]+'); errs=$(( errs + ${e:-0} ))
    rm -f "$o"
  done
  printf "  %-9s %-12s %-14s %-12s  errors=%s\n" "$N" "$PER" "$(printf '%d writes/s' "$agg")" "$(( agg / N ))/s" "$errs"
done

echo
echo "reading it: per-app stays ~flat (each app's single writer); aggregate scales with"
echo "the app count — horizontal write throughput by adding apps, not connections."
