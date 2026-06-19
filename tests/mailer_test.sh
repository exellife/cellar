#!/usr/bin/env bash
# Mailer end-to-end (no DB): point pgforge's SMTP at a local mock sink, send a
# test mail via the send-test-mail CLI, and assert the captured message is a
# well-formed RFC 5322 email. mailer_test.sh <pgforge-binary>
set -euo pipefail

BIN="${1:?usage: mailer_test.sh <pgforge-binary>}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"

PORT="$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")"
CAP="$(mktemp)"
python3 "$DIR/tests/mock_smtp.py" "$PORT" "$CAP" >/dev/null 2>&1 &
MOCK=$!
# The mock is one-shot and may already be gone by exit; `|| true` keeps the
# failing kill from tripping `set -e` inside the trap.
trap 'kill $MOCK 2>/dev/null || true; rm -f "$CAP"' EXIT

# Wait for the sink to be listening (probe connects + closes; the mock loops).
for _ in $(seq 1 50); do
  python3 -c "import socket,sys; sys.exit(0 if socket.socket().connect_ex(('127.0.0.1',$PORT))==0 else 1)" && break
  sleep 0.1
done

TO="alice@example.com"
PGF_SMTP_URL="smtp://127.0.0.1:$PORT" PGF_SMTP_TLS=none \
  PGF_MAIL_FROM="noreply@pgforge.test" PGF_MAIL_FROM_NAME="pgforge" \
  PGF_LOG_LEVEL=warn "$BIN" send-test-mail "$TO" || { echo "send-test-mail exited non-zero"; exit 1; }

# Give the sink a moment to flush the capture file.
for _ in $(seq 1 30); do [ -s "$CAP" ] && break; sleep 0.1; done

fail=0
chk() { if grep -qiE "$2" "$CAP"; then echo "  ok    $1"; else echo "  FAIL  $1"; fail=1; fi; }
chk "To header"        "^To: <$TO>"
chk "From has address" "^From: .*<noreply@pgforge\.test>"
chk "From has name"    "^From: pgforge "
chk "Subject header"   "^Subject: pgforge test email"
chk "Date header"      "^Date: .* \+0000"
chk "MIME content-type" "^Content-Type: text/plain; charset=UTF-8"
chk "Message-ID header" "^Message-ID: <.*@pgforge\.test>"
chk "body delivered"   "SMTP configuration works"

[ "$fail" = 0 ] && echo "MAILER PASS" || { echo "MAILER FAIL"; echo "--- captured ---"; cat "$CAP"; exit 1; }
