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

# Keep the .run DB schema current across restarts. run.sh re-copies hooks/policies on
# every start but NOT the schema — so a .run DB provisioned before a schema change would
# drift ("no such table: …"). Copy the migrations and apply any pending ones (idempotent;
# a no-op on a fresh/current DB). The server isn't started yet → no write-lock contention.
if [ -d "$HERE/migrations" ]; then
  rm -rf "$BUNDLE/migrations"; cp -r "$HERE/migrations" "$BUNDLE/migrations"
  if ! CEL_APPS_DIR="$APPS" "$BIN" migrate "$APP"; then
    echo "!! cellar migrate failed (see above) — refusing to start on an un-migrated DB" >&2; exit 1
  fi
  # migrate snapshots the DB before each apply; keep only the newest 5 so .backups/ stays tidy.
  ls -1dt "$BUNDLE/.backups"/pre-migrate-* 2>/dev/null | tail -n +6 | xargs -r rm -f
fi

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

# Dev mailer: unless a real CEL_SMTP_URL is set, boot a local mock SMTP sink so
# email flows (the 6-digit verification code, password reset) actually "send" to a
# capturable file — the frontend can read the latest verify code from it:
#   grep -o 'code is: [0-9]\{6\}' "$MAIL_CAP" | tail -1
MOCK_PID=""; MAIL_CAP=""
if [ -z "${CEL_SMTP_URL:-}" ] && [ -f "$HERE/../../tests/mock_smtp.py" ]; then
  MAIL_CAP="$(mktemp /tmp/classifieds-mail.XXXXXX)"
  # A persistent mock SMTP sink (accept loop) so every signup's 6-digit code is
  # captured (appended to $MAIL_CAP). Runs until this script exits (trap below).
  python3 "$HERE/../../tests/mock_smtp.py" --forever "$((PORT + 1))" "$MAIL_CAP" >/dev/null 2>&1 &
  MOCK_PID=$!
  export CEL_SMTP_URL="smtp://127.0.0.1:$((PORT + 1))" CEL_SMTP_TLS=none \
         CEL_MAIL_FROM="no-reply@classifieds.local" CEL_MAIL_CAPTURE="$MAIL_CAP"
  # Auto-login on register (return a session token) so the client can hit the
  # authenticated verify endpoint right after signup — matches the frontend flow.
  export CEL_REGISTER_AUTOLOGIN=1
fi

env CEL_PORT="$PORT" CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=info "$BIN" >/tmp/classifieds.log 2>&1 &
SRV=$!
trap '[ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null; kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null' EXIT
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done

# Seed the autonomous daily expiry sweep (idempotent — enqueue_job dedups
# recurring jobs by type, so re-running this never piles up duplicates).
ADMIN_TOK=$(curl -s -X POST -H "Host: $APP" -H 'Content-Type: application/json' \
  -d "{\"email\":\"$ADMIN_EMAIL\",\"password\":\"$ADMIN_PW\"}" \
  "http://127.0.0.1:$PORT/auth/login" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
for jt in '{"type":"expire_listings","repeat_every":86400}' '{"type":"match_saved_searches","repeat_every":900}' '{"type":"rollup_interest","repeat_every":86400}'; do
  [ -n "$ADMIN_TOK" ] && curl -s -o /dev/null -X POST -H "Host: $APP" -H "Authorization: Bearer $ADMIN_TOK" \
    -H 'Content-Type: application/json' -d "$jt" "http://127.0.0.1:$PORT/rpc/enqueue_job"
done

# Dev/test logins (idempotent): a non-admin seller + buyer so the frontend has
# stable role=user credentials out of the box. Created via the superuser-only
# POST /auth/users; a 409 (already exists) on re-run is harmless and ignored.
# NOTE: dev only — remove/rotate before any real deployment.
for uc in seller buyer; do
  [ -n "$ADMIN_TOK" ] && curl -s -o /dev/null -X POST -H "Host: $APP" -H "Authorization: Bearer $ADMIN_TOK" \
    -H 'Content-Type: application/json' \
    -d "{\"email\":\"$uc@classifieds.local\",\"password\":\"test1234\",\"role\":\"user\"}" \
    "http://127.0.0.1:$PORT/auth/users"
done

# Mark the dev seller+buyer email-verified so the (default-on) contact gate lets
# them post/reveal/chat out of the box; a NEW self-signup still exercises the gate +
# the 6-digit code flow (code lands in the mock-mail capture above).
sqlite3 "$DB" "UPDATE cel_users SET email_verified_at = COALESCE(email_verified_at, strftime('%s','now')) \
  WHERE email IN ('seller@classifieds.local','buyer@classifieds.local');" 2>/dev/null || true

cat <<EOF

  classifieds is live →  http://localhost:$PORT/
  admin              →  $ADMIN_EMAIL / $ADMIN_PW
  seller (user)      →  seller@classifieds.local / test1234
  buyer  (user)      →  buyer@classifieds.local / test1234

  Catalog seeded: KG geo tree + a starter taxonomy (transport/realestate/…).
  Try it:
    curl -s 'http://localhost:$PORT/api/category?where={"parent_id":{"is":null}}' -H 'Host: $APP'
    curl -s 'http://localhost:$PORT/api/category_attribute?where={"category_id":{"eq":"cat-cars"}}' -H 'Host: $APP'

  Posting a listing needs a logged-in user (see policies.json). Ctrl-C to stop.
  Logs: /tmp/classifieds.log
  Dev mail sink: ${MAIL_CAP:-<real SMTP configured>}  (6-digit verify codes land here)
  Verify gate: post/reveal/chat require a verified email (dev seller/buyer are pre-verified);
    a new signup auto-logs-in + gets a code in the sink — read the latest:
    grep -oE 'Jarchy: [0-9]{6}' "${MAIL_CAP:-…}" | tail -1

EOF
wait "$SRV"
