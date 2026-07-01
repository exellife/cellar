#!/usr/bin/env bash
# Auth hardening: session tokens stored hashed at rest, revoke-sessions CLI kills
# them, and over-length passwords are rejected. Boots its own server against a
# throwaway per-app SQLite database (needs the binary for both serving and the
# CLI). auth_hardening_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: auth_hardening_test.sh <cellar-binary>}"
TMP="$(mktemp -d)"; DB="$TMP/data.db"
SQ() { sqlite3 "$DB" "$1"; }   # scalar query helper (WAL -> concurrent reads ok)
PORT=$(python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()")

CEL_PORT=$PORT CEL_DATA_DB="$DB" CEL_LOG_LEVEL=warn \
  CEL_AUTH_RATELIMIT=0 CEL_SEED_USERS="admin@cellar.dev:s3cret-admin:admin" \
  "$BIN" >/tmp/ah_$$.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null; rm -f /tmp/ah_$$.log; rm -rf "$TMP"' EXIT
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/health" && break; sleep 0.1; done

fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok    $1 ($2)"; else echo "  FAIL  $1: got '$2' want '$3'"; fail=1; fi; }
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

TOKEN=$(curl -s -X POST -H 'Content-Type: application/json' \
        -d '{"email":"admin@cellar.dev","password":"s3cret-admin"}' \
        "http://127.0.0.1:$PORT/auth/login" | python3 -c "import sys,json;print(json.load(sys.stdin)['token'])")
HASH=$(python3 -c "import hashlib,sys;print(hashlib.sha256('$TOKEN'.encode()).hexdigest())")

chk "raw token NOT at rest"  "$(SQ "SELECT count(*) FROM cel_sessions WHERE token='$TOKEN'")" "0"
chk "hashed token at rest"   "$(SQ "SELECT count(*) FROM cel_sessions WHERE token='$HASH'")"  "1"
chk "token authenticates"    "$(code -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT/schema")" "200"

# revoke-sessions CLI kills the token (cache is off -> immediate)
CEL_DATA_DB="$DB" CEL_LOG_LEVEL=error \
  "$BIN" revoke-sessions admin@cellar.dev >/dev/null 2>&1
chk "revoked token rejected"  "$(code -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT/schema")" "401"

# over-length password rejected before Argon2id
LONGPW=$(python3 -c "print('a'*200)")
chk "long password -> 400"   "$(code -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"x@y.z\",\"password\":\"$LONGPW\",\"role\":\"admin\"}" \
        "http://127.0.0.1:$PORT/auth/register")" "400"

# passwd CLI: out-of-band reset — old password stops working, new one logs in
CEL_DATA_DB="$DB" CEL_LOG_LEVEL=error "$BIN" passwd admin@cellar.dev n3w-passw0rd >/dev/null 2>&1
chk "old password -> 401"    "$(code -X POST -H 'Content-Type: application/json' \
        -d '{"email":"admin@cellar.dev","password":"s3cret-admin"}' \
        "http://127.0.0.1:$PORT/auth/login")" "401"
chk "new password -> 200"    "$(code -X POST -H 'Content-Type: application/json' \
        -d '{"email":"admin@cellar.dev","password":"n3w-passw0rd"}' \
        "http://127.0.0.1:$PORT/auth/login")" "200"
# a passwd for an email with no password account is a clean failure (exit 1), not a crash
if CEL_DATA_DB="$DB" CEL_LOG_LEVEL=error "$BIN" passwd ghost@nowhere.z whatever123 >/dev/null 2>&1
then rc=0; else rc=$?; fi
chk "passwd unknown email -> exit 1" "$rc" "1"

[ "$fail" = 0 ] && echo "AUTH HARDENING PASS" || { echo "AUTH HARDENING FAIL"; exit 1; }
