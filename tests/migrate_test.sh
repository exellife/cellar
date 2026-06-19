#!/usr/bin/env bash
# Verify the migration runner: core bootstrap, opt-in demo, idempotency, and the
# checksum immutability guard. Usage: migrate_test.sh <cellar-binary>
set -euo pipefail

BIN="${1:?usage: migrate_test.sh <binary>}"
H="${CEL_DB_HOST:-localhost}"
U="${CEL_DB_USER:-postgres}"
DB="cel_migtest"

psql -h "$H" -U "$U" -tc "SELECT 1 FROM pg_database WHERE datname='$DB'" | grep -q 1 \
  && psql -h "$H" -U "$U" -c "DROP DATABASE $DB" >/dev/null
psql -h "$H" -U "$U" -c "CREATE DATABASE $DB" >/dev/null

export CEL_DB_NAME="$DB" CEL_DB_HOST="$H" CEL_DB_USER="$U" CEL_LOG_LEVEL=warn
cleanup() { psql -h "$H" -U "$U" -c "DROP DATABASE $DB" >/dev/null 2>&1 || true; }
trap cleanup EXIT
fail() { echo "FAIL: $1"; exit 1; }
exists() { [ "$(psql -h "$H" -U "$U" -d "$DB" -tAc "SELECT to_regclass('public.$1') IS NOT NULL")" = "t" ]; }
count()  { psql -h "$H" -U "$U" -d "$DB" -tAc "SELECT count(*) FROM cel_migrations"; }

# core migrate -> only core tables; demo tables absent
"$BIN" migrate >/dev/null || fail "core migrate exited non-zero"
for t in cel_migrations cel_users cel_sessions cel_identities cel_mfa cel_password_resets cel_email_verifications; do exists "$t" || fail "core table $t missing"; done
exists products && fail "demo table 'products' should NOT exist after a core-only migrate"
[ "$(count)" = "8" ] || fail "expected 8 core migrations recorded, got $(count)"

# --demo applies sample data
"$BIN" migrate --demo >/dev/null || fail "demo migrate exited non-zero"
for t in products categories notes; do exists "$t" || fail "demo table $t missing after --demo"; done
[ "$(count)" = "10" ] || fail "expected 10 migrations (8 core + 2 demo), got $(count)"

# idempotent
"$BIN" migrate --demo >/dev/null || fail "second migrate exited non-zero"
[ "$(count)" = "10" ] || fail "second run changed the migration count ($(count))"

# checksum guard: tamper a recorded checksum -> next migrate must refuse
psql -h "$H" -U "$U" -d "$DB" -c \
  "UPDATE cel_migrations SET checksum='tampered' WHERE name='001_users_sessions.sql'" >/dev/null
if "$BIN" migrate >/dev/null 2>&1; then fail "checksum mismatch was NOT refused"; fi

echo "migrate test: PASS (core/demo split, idempotent, checksum guard)"
