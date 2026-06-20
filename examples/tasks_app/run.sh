#!/usr/bin/env bash
# Tasklets — provision the example bundle and boot cellar to serve it.
#
#   examples/tasks_app/run.sh [cellar-binary] [port]
#
# Builds a throwaway bundle under examples/tasks_app/.run/ named "localhost" (so a
# browser at http://localhost:<port> routes to it via the Host header), applies the
# schema, installs hooks.lua + policies.json + public/, seeds a few demo tasks, and
# starts the server. Ctrl-C to stop; the bundle persists between runs (delete .run/
# to reset).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${1:-$HERE/../../build-cmake/cellar}"
PORT="${2:-8080}"
HOSTNAME_APP="localhost"
ADMIN_EMAIL="admin@tasks.local"
ADMIN_PW="tasklets"

[ -x "$BIN" ] || { echo "cellar binary not found/executable: $BIN" >&2
                   echo "build it first (cmake --build build-cmake) or pass the path as \$1" >&2; exit 1; }
command -v sqlite3 >/dev/null || { echo "this script needs the sqlite3 CLI to load the schema" >&2; exit 1; }

APPS="$HERE/.run"
BUNDLE="$APPS/$HOSTNAME_APP"
DB="$BUNDLE/data.db"

if [ ! -f "$DB" ]; then
  echo "== provisioning bundle ($HOSTNAME_APP) =="
  mkdir -p "$APPS"
  CEL_APPS_DIR="$APPS" "$BIN" provision "$HOSTNAME_APP" "$ADMIN_EMAIL" "$ADMIN_PW"

  echo "== applying schema + installing hooks/policies/public =="
  sqlite3 "$DB" < "$HERE/schema.sql"
  cp "$HERE/hooks.lua"     "$BUNDLE/hooks.lua"
  cp "$HERE/policies.json" "$BUNDLE/policies.json"
  rm -rf "$BUNDLE/public"
  cp -r "$HERE/public"     "$BUNDLE/public"

  echo "== seeding a few demo tasks for the admin =="
  ADMIN_ID="$(sqlite3 "$DB" "SELECT id FROM cel_users WHERE email='$ADMIN_EMAIL' LIMIT 1;")"
  sqlite3 "$DB" <<SQL
INSERT INTO tasks(title, status, priority, owner_id) VALUES
  ('Read the Tasklets README', 'todo',  2, '$ADMIN_ID'),
  ('Open a second window to see realtime', 'todo', 4, '$ADMIN_ID'),
  ('Drag a task across the board', 'doing', 3, '$ADMIN_ID'),
  ('Ship something on cellar', 'done', 5, '$ADMIN_ID');
SQL
else
  # keep code/policy in sync with the repo on every run (data is preserved)
  cp "$HERE/hooks.lua"     "$BUNDLE/hooks.lua"
  cp "$HERE/policies.json" "$BUNDLE/policies.json"
  rm -rf "$BUNDLE/public"; cp -r "$HERE/public" "$BUNDLE/public"
  echo "== reusing existing bundle at $BUNDLE (delete .run/ to reset) =="
fi

cat <<EOF

  Tasklets is live →  http://localhost:$PORT/
  seeded admin     →  $ADMIN_EMAIL / $ADMIN_PW   (superuser: sees all tasks)
  or "Create account" in the UI for a private member board.

  Watch the network tab: /auth/login, /api/tasks, /rpc/board_stats, and the WS.
  Ctrl-C to stop.

EOF

exec env CEL_PORT="$PORT" CEL_APPS_DIR="$APPS" CEL_AUTH_RATELIMIT=0 CEL_API_RATELIMIT=0 \
     CEL_LOG_LEVEL=info "$BIN"
