#!/usr/bin/env bash
# TOTP 2FA end-to-end: boot pgforge with PGF_MFA=optional and a dedicated test
# user, then run the enroll/confirm/two-step-login/disable flow. mfa_test.sh <bin>
set -euo pipefail

BIN="${1:?usage: mfa_test.sh <pgforge-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
H="${PGF_DB_HOST:-localhost}"; U="${PGF_DB_USER:-postgres}"; DB="${PGF_DB_NAME:-pgforge}"

# Start clean: drop any leftover enrollment for the test user (re-runs).
psql -h "$H" -U "$U" -d "$DB" -c \
  "DELETE FROM pgf_mfa WHERE user_id=(SELECT id FROM pgf_users WHERE email='mfatest@pgforge.dev')" \
  >/dev/null 2>&1 || true

export PGF_MFA=optional
export PGF_SEED_USERS="admin@pgforge.dev:s3cret-admin:admin;mfatest@pgforge.dev:mfatest-pw:admin"
exec python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/mfa_test.py"
