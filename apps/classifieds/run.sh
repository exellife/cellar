#!/usr/bin/env bash
# classifieds — provision the bundle and boot cellar to serve the catalog API.
#   apps/classifieds/run.sh [cellar-binary] [port]
# Single global catalog (one app, one DB). Delete .run/ to reset.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${1:-$HERE/../../build-cmake/cellar}"
PORT="${2:-8080}"
APP=localhost
ADMIN_EMAIL="admin@classifieds.local"
ADMIN_PW="classifieds01"

[ -x "$BIN" ] || { echo "build cellar first (cmake --build build-cmake) or pass the path as \$1" >&2; exit 1; }
command -v sqlite3 >/dev/null || { echo "needs the sqlite3 CLI" >&2; exit 1; }

APPS="$HERE/.run"; BUNDLE="$APPS/$APP"; DB="$BUNDLE/data.db"
if [ ! -f "$DB" ]; then
  echo "== provisioning catalog bundle =="
  mkdir -p "$APPS"
  CEL_APPS_DIR="$APPS" "$BIN" provision "$APP" "$ADMIN_EMAIL" "$ADMIN_PW" >/dev/null
  sqlite3 "$DB" < "$HERE/schema.sql"
  sqlite3 "$DB" < "$HERE/seed.sql"
fi
cp "$HERE/hooks.lua" "$BUNDLE/hooks.lua"
cp "$HERE/policies.json" "$BUNDLE/policies.json"

env CEL_PORT="$PORT" CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=info "$BIN" >/tmp/classifieds.log 2>&1 &
SRV=$!
trap 'kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null' EXIT
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done

cat <<EOF

  classifieds is live →  http://localhost:$PORT/
  admin              →  $ADMIN_EMAIL / $ADMIN_PW

  Catalog seeded: KG geo tree + a starter taxonomy (transport/realestate/…).
  Try it:
    curl -s 'http://localhost:$PORT/api/category?where={"parent_id":{"is":null}}' -H 'Host: $APP'
    curl -s 'http://localhost:$PORT/api/category_attribute?where={"category_id":{"eq":"cat-cars"}}' -H 'Host: $APP'

  Posting a listing needs a logged-in user (see policies.json). Ctrl-C to stop.
  Logs: /tmp/classifieds.log

EOF
wait "$SRV"
