#!/usr/bin/env bash
# H-5: by default /auth/register must NOT be an account-enumeration oracle — a new
# email and an already-registered email return the SAME uniform 202 (no token, no
# distinguishing 409). No PGF_REGISTER_AUTOLOGIN here = the secure default.
# register_enum_test.sh <pgforge-binary>
set -euo pipefail

BIN="${1:?usage: register_enum_test.sh <pgforge-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
H="${PGF_DB_HOST:-localhost}"; U="${PGF_DB_USER:-postgres}"; DB="${PGF_DB_NAME:-pgforge}"

clean() {
  psql -h "$H" -U "$U" -d "$DB" -c \
    "DELETE FROM pgf_users WHERE email LIKE 'enum-%@test.local'" >/dev/null 2>&1 || true
}
trap clean EXIT
clean

export PGF_POLICY_FILE="$DIR/config/policies.taxi.example.json"   # rider self-registers
rc=0
python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/register_enum.py" || rc=$?
exit $rc
