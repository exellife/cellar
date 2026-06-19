#!/usr/bin/env bash
# Account lockout end-to-end: boot with CEL_AUTH_LOCKOUT=3/2 (lock after 3 fails,
# 2s window/lock) and the per-IP limiter off, then drive lock -> locked-even-with-
# correct-pw -> auto-unlock after the window -> admin `unlock` CLI. lockout_test.sh <bin>
set -euo pipefail

BIN="${1:?usage: lockout_test.sh <cellar-binary>}"
H="${CEL_DB_HOST:-localhost}"; U="${CEL_DB_USER:-postgres}"; DB="${CEL_DB_NAME:-cellar}"
PORT=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")
USER="lockme@test.local"; PW="correct-pw-1"

clean_db() { psql -h "$H" -U "$U" -d "$DB" -c "DELETE FROM cel_users WHERE email='$USER'" >/dev/null 2>&1 || true; }
clean_db

CEL_PORT=$PORT CEL_DB_HOST=$H CEL_DB_USER=$U CEL_DB_NAME=$DB CEL_LOG_LEVEL=warn \
  CEL_AUTH_RATELIMIT=0 CEL_AUTH_LOCKOUT=3/2 CEL_SEED_USERS="$USER:$PW:editor" \
  "$BIN" >/tmp/lk_$$.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true; rm -f /tmp/lk_$$.log; clean_db' EXIT
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done

fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok    $1 ($2)"; else echo "  FAIL  $1: got '$2' want '$3'"; fail=1; fi; }
post() { curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
           -d "{\"email\":\"$USER\",\"password\":\"$1\"}" "http://127.0.0.1:$PORT/auth/login"; }
bad()  { post "WRONG-pw"; }
good() { post "$PW"; }

# three failures reach the limit (the third sets the lock)
chk "fail 1 -> 401" "$(bad)" "401"
chk "fail 2 -> 401" "$(bad)" "401"
chk "fail 3 -> 401" "$(bad)" "401"
# Now locked. L-1: a locked account is NOT advertised — responses stay a uniform
# 401 (indistinguishable from a wrong password / unknown user), so the lock is
# observed only by the CORRECT password being refused (then accepted after unlock).
chk "locked attempt -> 401"          "$(bad)"  "401"
chk "correct pw while locked -> 401" "$(good)" "401"

# the lock auto-expires after the window; a correct password then works + resets
sleep 2.5
chk "correct pw after window -> 200" "$(good)" "200"
# a single failure after the reset does not lock (fresh streak)
chk "one fail post-reset -> 401" "$(bad)" "401"

# lock again, then clear it with the admin CLI
bad >/dev/null; bad >/dev/null
chk "re-locked: correct pw -> 401" "$(good)" "401"
CEL_DB_HOST=$H CEL_DB_USER=$U CEL_DB_NAME=$DB CEL_LOG_LEVEL=error "$BIN" unlock "$USER" >/dev/null 2>&1
chk "correct pw after admin unlock -> 200" "$(good)" "200"

[ "$fail" = 0 ] && echo "LOCKOUT PASS" || { echo "LOCKOUT FAIL"; exit 1; }
