#!/usr/bin/env bash
# Offline Notes — provision the bundle and boot cellar to serve it.
#   examples/offline_notes/run.sh [cellar-binary] [port]
# Then open http://localhost:<port>/ in TWO tabs (= two devices) and sign in as the
# same user. Delete .run/ to reset. Ctrl-C to stop.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${1:-$HERE/../../build-cmake/cellar}"
PORT="${2:-8080}"
APP=localhost
MEMBER_EMAIL="me@notes.local"
MEMBER_PW="notes-secret"

[ -x "$BIN" ] || { echo "cellar binary not found: $BIN (build it, or pass the path as \$1)" >&2; exit 1; }
command -v sqlite3 >/dev/null || { echo "this script needs the sqlite3 CLI" >&2; exit 1; }

APPS="$HERE/.run"; BUNDLE="$APPS/$APP"; DB="$BUNDLE/data.db"
if [ ! -f "$DB" ]; then
  echo "== provisioning bundle =="
  mkdir -p "$APPS"
  CEL_APPS_DIR="$APPS" "$BIN" provision "$APP" admin@notes.local adminpw >/dev/null
  sqlite3 "$DB" < "$HERE/schema.sql"
fi
# keep code/policy/front-end in sync with the repo on every run (data preserved)
cp "$HERE/hooks.lua" "$BUNDLE/hooks.lua"
cp "$HERE/policies.json" "$BUNDLE/policies.json"
rm -rf "$BUNDLE/public"; cp -r "$HERE/public" "$BUNDLE/public"

env CEL_PORT="$PORT" CEL_APPS_DIR="$APPS" CEL_AUTH_RATELIMIT=0 CEL_API_RATELIMIT=0 \
    CEL_LOG_LEVEL=info "$BIN" >/tmp/offline_notes.log 2>&1 &
SRV=$!
trap 'kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null' EXIT
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done

# pre-seed the demo member so "Sign in" works immediately (idempotent: uniform 202)
curl -s -o /dev/null -H "Host: $APP" -X POST -H 'Content-Type: application/json' \
     -d "{\"email\":\"$MEMBER_EMAIL\",\"password\":\"$MEMBER_PW\",\"role\":\"member\"}" \
     "http://127.0.0.1:$PORT/auth/register" || true

cat <<EOF

  Offline Notes is live →  http://localhost:$PORT/
  member (prefilled)    →  $MEMBER_EMAIL / $MEMBER_PW

  Try it: open TWO tabs. In each, toggle "online" OFF, edit some notes (they queue —
  watch "N pending"), then toggle ONE back ON to push+pull. Edit the SAME note in both
  while offline, then sync both → the resolve() hook (most-recent edit wins) settles it.
  Ctrl-C to stop.

EOF
wait "$SRV"
