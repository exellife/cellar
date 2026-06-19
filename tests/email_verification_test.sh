#!/usr/bin/env bash
# Email-verification end-to-end: boot cellar with SMTP pointed at a local mock
# sink and a self-register policy, register a user (token emailed + captured),
# redeem it, and watch email_verified flip. email_verification_test.sh <binary>
set -euo pipefail

BIN="${1:?usage: email_verification_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
H="${CEL_DB_HOST:-localhost}"; U="${CEL_DB_USER:-postgres}"; DB="${CEL_DB_NAME:-cellar}"
EMAIL="verifyme@test.local"

clean_db() { psql -h "$H" -U "$U" -d "$DB" -c "DELETE FROM cel_users WHERE email='$EMAIL'" >/dev/null 2>&1 || true; }
clean_db

PORT="$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")"
CAP="$(mktemp)"
python3 "$DIR/tests/mock_smtp.py" "$PORT" "$CAP" >/dev/null 2>&1 &
MOCK=$!
trap 'kill $MOCK 2>/dev/null || true; rm -f "$CAP"; clean_db' EXIT
for _ in $(seq 1 50); do
  python3 -c "import socket,sys; sys.exit(0 if socket.socket().connect_ex(('127.0.0.1',$PORT))==0 else 1)" && break
  sleep 0.1
done

# taxi example policy makes 'rider' self-registerable (the default signup role).
export CEL_POLICY_FILE="$DIR/config/policies.taxi.example.json"
export CEL_SMTP_URL="smtp://127.0.0.1:$PORT" CEL_SMTP_TLS=none
export CEL_MAIL_FROM="noreply@cellar.test" CEL_APP_URL="https://app.test"
export CEL_MAIL_CAPTURE="$CAP" VERIFY_EMAIL="$EMAIL"
export CEL_REGISTER_AUTOLOGIN=1   # this flow registers then uses the returned token

rc=0
python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/email_verification_test.py" || rc=$?
exit $rc
