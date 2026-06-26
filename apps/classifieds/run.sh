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

# --- Auth (A1.8) -----------------------------------------------------------
# Password + session login works out of the box (self_register=user). The engine
# also supports OAuth/OIDC and email magic-link — both CONFIG-ONLY (no bundle
# code); a federated/new user lands as role `user` (the first self_register role).
#
# Google sign-in: export these before running (the client sends Google's ID token
# to POST /auth/oauth; cellar verifies it against the JWKS, no email send needed):
#   export CEL_OAUTH_GOOGLE_ISSUER=https://accounts.google.com
#   export CEL_OAUTH_GOOGLE_JWKS=https://www.googleapis.com/oauth2/v3/certs
#   export CEL_OAUTH_GOOGLE_CLIENT_ID=<your-google-client-id>
#
# Email magic-link / verify-email: DEFERRED — needs a domain + a transactional
# provider (SES/Postmark/Resend…) + SPF/DKIM/DMARC; do NOT direct-send from this
# box (Gmail/Outlook reject it). See dist/cellar.env.example (DELIVERABILITY).
# Inherited CEL_OAUTH_* / CEL_SMTP_* from the environment pass through below.

env CEL_PORT="$PORT" CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=info "$BIN" >/tmp/classifieds.log 2>&1 &
SRV=$!
trap 'kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null' EXIT
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done

# Seed the autonomous daily expiry sweep (idempotent — enqueue_job dedups
# recurring jobs by type, so re-running this never piles up duplicates).
ADMIN_TOK=$(curl -s -X POST -H "Host: $APP" -H 'Content-Type: application/json' \
  -d "{\"email\":\"$ADMIN_EMAIL\",\"password\":\"$ADMIN_PW\"}" \
  "http://127.0.0.1:$PORT/auth/login" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
for jt in '{"type":"expire_listings","repeat_every":86400}' '{"type":"match_saved_searches","repeat_every":900}'; do
  [ -n "$ADMIN_TOK" ] && curl -s -o /dev/null -X POST -H "Host: $APP" -H "Authorization: Bearer $ADMIN_TOK" \
    -H 'Content-Type: application/json' -d "$jt" "http://127.0.0.1:$PORT/rpc/enqueue_job"
done

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
