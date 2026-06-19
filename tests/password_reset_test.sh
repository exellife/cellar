#!/usr/bin/env bash
# Password-reset end-to-end: boot cellar with SMTP pointed at a local mock sink,
# request a reset (the token is emailed + captured), redeem it, and verify the new
# password works / old one + old session don't. password_reset_test.sh <binary>
set -euo pipefail

BIN="${1:?usage: password_reset_test.sh <cellar-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
H="${CEL_DB_HOST:-localhost}"; U="${CEL_DB_USER:-postgres}"; DB="${CEL_DB_NAME:-cellar}"
USER="pwreset@test.local"

clean_db() { psql -h "$H" -U "$U" -d "$DB" -c "DELETE FROM cel_users WHERE email='$USER'" >/dev/null 2>&1 || true; }
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

export CEL_SMTP_URL="smtp://127.0.0.1:$PORT" CEL_SMTP_TLS=none
export CEL_MAIL_FROM="noreply@cellar.test" CEL_APP_URL="https://app.test"
export CEL_SEED_USERS="admin@cellar.dev:s3cret-admin:admin;$USER:oldpassword1:editor"
export CEL_MAIL_CAPTURE="$CAP" RESET_USER="$USER"
export CEL_AUTH_LOCKOUT=3/60   # L-5: lock the account, then prove the reset clears it

# Not exec: let the EXIT trap clean up the mock + temp file + db row.
rc=0
python3 "$DIR/tests/run_with_server.py" "$BIN" "$DIR/tests/password_reset_test.py" || rc=$?
exit $rc
