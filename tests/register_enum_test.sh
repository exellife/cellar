#!/usr/bin/env bash
# H-5: by default /auth/register must NOT be an account-enumeration oracle — a new
# email and an already-registered email return the SAME uniform 202 (no token, no
# distinguishing 409). No CEL_REGISTER_AUTOLOGIN here = the secure default.
# register_enum_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: register_enum_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
H="${CEL_DB_HOST:-localhost}"; U="${CEL_DB_USER:-postgres}"; DB="${CEL_DB_NAME:-cellar}"

clean() {
  psql -h "$H" -U "$U" -d "$DB" -c \
    "DELETE FROM cel_users WHERE email LIKE 'enum-%@test.local'" >/dev/null 2>&1 || true
}
trap clean EXIT
clean

export CEL_POLICY_FILE="$DIR/config/policies.taxi.example.json"   # rider self-registers
rc=0
python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/register_enum.py" || rc=$?
exit $rc
