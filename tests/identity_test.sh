#!/usr/bin/env bash
# Identity split (#auth, PLAN §7e): credentials live in cel_identities, not in
# cel_users. Proves the password_hash column is gone, a seeded user has a
# 'password' identity, login resolves THROUGH that identity, and an admin-created
# user gets a working identity it can log in with. Boots its own server against a
# throwaway per-app SQLite database (needs the binary). identity_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: identity_test.sh <cellar-binary>}"
TMP="$(mktemp -d)"; DB="$TMP/data.db"
SQ() { sqlite3 "$DB" "$1"; }   # scalar query helper (WAL -> concurrent reads ok)
PORT=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")

ADMIN="idtest-admin@cellar.dev"
NEWUSER="idtest-user@cellar.dev"

CEL_PORT=$PORT CEL_DATA_DB="$DB" CEL_LOG_LEVEL=warn \
  CEL_AUTH_RATELIMIT=0 CEL_SEED_USERS="$ADMIN:s3cret-admin:admin" \
  "$BIN" >/tmp/id_$$.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null; rm -f /tmp/id_$$.log; rm -rf "$TMP"' EXIT
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done

fail=0
chk()  { if [ "$2" = "$3" ]; then echo "  ok    $1 ($2)"; else echo "  FAIL  $1: got '$2' want '$3'"; fail=1; fi; }
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
login_token() {
  curl -s -X POST -H 'Content-Type: application/json' -d "{\"email\":\"$1\",\"password\":\"$2\"}" \
    "http://127.0.0.1:$PORT/auth/login" | python3 -c "import sys,json;print(json.load(sys.stdin).get('token',''))"
}

# ---- schema: the credential moved out of cel_users ----
chk "password_hash column dropped" \
    "$(SQ "SELECT count(*) FROM pragma_table_info('cel_users') WHERE name='password_hash'")" "0"
chk "seeded admin has a password identity" \
    "$(SQ "SELECT count(*) FROM cel_identities WHERE provider='password' AND provider_uid='$ADMIN'")" "1"
# the secret is the argon2id hash, parked in the identity (not cel_users)
chk "identity secret is argon2id" \
    "$(SQ "SELECT count(*) FROM cel_identities WHERE provider_uid='$ADMIN' AND secret LIKE '\$argon2id\$%'")" "1"

# ---- login resolves through the identity ----
TOKEN=$(login_token "$ADMIN" "s3cret-admin")
chk "admin login via identity" "$([ -n "$TOKEN" ] && echo yes || echo no)" "yes"

# ---- admin-created user gets a working identity ----
chk "admin creates user -> 201" \
    "$(code -X POST -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
        -d "{\"email\":\"$NEWUSER\",\"password\":\"newuser-pw\",\"role\":\"viewer\"}" \
        "http://127.0.0.1:$PORT/auth/users")" "201"
chk "created user has a password identity" \
    "$(SQ "SELECT count(*) FROM cel_identities WHERE provider='password' AND provider_uid='$NEWUSER'")" "1"
chk "created user can log in" \
    "$(code -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$NEWUSER\",\"password\":\"newuser-pw\"}" "http://127.0.0.1:$PORT/auth/login")" "200"
# anti-enumeration / verify path intact: a wrong password is still 401
chk "wrong password -> 401" \
    "$(code -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$NEWUSER\",\"password\":\"WRONG\"}" "http://127.0.0.1:$PORT/auth/login")" "401"

[ "$fail" = 0 ] && echo "IDENTITY SPLIT PASS" || { echo "IDENTITY SPLIT FAIL"; exit 1; }
