#!/usr/bin/env bash
# TOTP 2FA end-to-end: boot cellar with CEL_MFA=optional and a dedicated test
# user, then run the enroll/confirm/two-step-login/disable flow. mfa_test.sh <bin>
set -euo pipefail

BIN="${1:?usage: mfa_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
H="${CEL_DB_HOST:-localhost}"; U="${CEL_DB_USER:-postgres}"; DB="${CEL_DB_NAME:-cellar}"

# Start clean: drop any leftover enrollment for the test user (re-runs).
psql -h "$H" -U "$U" -d "$DB" -c \
  "DELETE FROM cel_mfa WHERE user_id=(SELECT id FROM cel_users WHERE email='mfatest@cellar.dev')" \
  >/dev/null 2>&1 || true

export CEL_MFA=optional
export CEL_SEED_USERS="admin@cellar.dev:s3cret-admin:admin;mfatest@cellar.dev:mfatest-pw:admin"
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/mfa_test.py"
