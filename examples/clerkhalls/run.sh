#!/usr/bin/env bash
# ClerkHalls — provision the org bundle and boot cellar to serve the SPA.
#   examples/clerkhalls/run.sh [cellar-binary] [port]
# One bundle = one organization. Open http://localhost:<port>/ (sign in prefilled).
# A second browser = a second device → live sync. Delete .run/ to reset.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${1:-$HERE/../../build-cmake/cellar}"
PORT="${2:-8080}"
APP=localhost
STAFF_EMAIL="owner@clerkhalls.local"
STAFF_PW="clerkhalls"

[ -x "$BIN" ] || { echo "build cellar first (cmake --build build-cmake) or pass the path as \$1" >&2; exit 1; }
command -v sqlite3 >/dev/null || { echo "needs the sqlite3 CLI" >&2; exit 1; }

APPS="$HERE/.run"; BUNDLE="$APPS/$APP"; DB="$BUNDLE/data.db"
if [ ! -f "$DB" ]; then
  echo "== provisioning org bundle =="
  mkdir -p "$APPS"
  CEL_APPS_DIR="$APPS" "$BIN" provision "$APP" admin@clerkhalls.local adminpw01 >/dev/null
  sqlite3 "$DB" < "$HERE/schema.sql"
fi
cp "$HERE/hooks.lua" "$BUNDLE/hooks.lua"
cp "$HERE/policies.json" "$BUNDLE/policies.json"
rm -rf "$BUNDLE/public"; cp -r "$HERE/public" "$BUNDLE/public"

env CEL_PORT="$PORT" CEL_APPS_DIR="$APPS" CEL_AUTH_RATELIMIT=0 CEL_API_RATELIMIT=0 \
    CEL_LOG_LEVEL=info "$BIN" >/tmp/clerkhalls.log 2>&1 &
SRV=$!
trap 'kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null' EXIT
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done
curl -s -o /dev/null -H "Host: $APP" -X POST -H 'Content-Type: application/json' \
     -d "{\"email\":\"$STAFF_EMAIL\",\"password\":\"$STAFF_PW\",\"role\":\"staff\"}" \
     "http://127.0.0.1:$PORT/auth/register" || true

cat <<EOF

  ClerkHalls is live →  http://localhost:$PORT/
  staff (prefilled)  →  $STAFF_EMAIL / $STAFF_PW

  Iteration 1: Venues & Rooms (full CRUD, offline-first, live sync). Bookings / Menu /
  Settings are stubs we'll fill next. Open a 2nd browser as another device to watch
  sync; toggle "online" off to queue edits offline. Ctrl-C to stop.

EOF
wait "$SRV"
