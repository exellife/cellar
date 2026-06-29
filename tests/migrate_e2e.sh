#!/usr/bin/env bash
# e2e: `cellar migrate` against a POPULATED DB — the data-safety guarantees.
#   - an existing, populated app (schema applied directly, no _schema_migrations)
#     is ADOPTED by an IF-NOT-EXISTS 0001 baseline without losing data;
#   - a real ALTER (0002) preserves rows + adds the column;
#   - a consistent PRE-migration backup is left behind and is restorable;
#   - re-running is idempotent.
# Usage: migrate_e2e.sh <cellar-binary>
set -u
BIN="${1:?usage: migrate_e2e.sh <cellar-binary>}"
command -v sqlite3 >/dev/null || { echo "SKIP: needs the sqlite3 CLI"; exit 0; }

fail=0
chk() { if [ "$2" = "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1 (got '$2' want '$3')"; fail=1; fi; }

APPS=$(mktemp -d "${TMPDIR:-/tmp}/cel-migrate-e2e-XXXXXX")
trap 'rm -rf "$APPS"' EXIT
ORG="$APPS/app"

CEL_APPS_DIR="$APPS" CEL_LOG_LEVEL=error "$BIN" provision app a@b.c pw123456 >/dev/null 2>&1

# Simulate an already-deployed, POPULATED app: schema applied directly, real rows,
# and NO _schema_migrations tracker yet (the srvlab adoption scenario).
sqlite3 "$ORG/data.db" "CREATE TABLE booking(id TEXT PRIMARY KEY, title TEXT NOT NULL);
                        INSERT INTO booking VALUES('b1','Wedding'),('b2','Conf'),('b3','Party');"

mkdir -p "$ORG/migrations"
printf 'CREATE TABLE IF NOT EXISTS booking(id TEXT PRIMARY KEY, title TEXT NOT NULL);\n' > "$ORG/migrations/0001_init.sql"
printf 'ALTER TABLE booking ADD COLUMN notes TEXT;\n'                                    > "$ORG/migrations/0002_notes.sql"

CEL_APPS_DIR="$APPS" "$BIN" migrate app >/dev/null 2>&1; rc=$?
chk "migrate exits 0"                 "$rc" "0"

# live DB: data preserved through baseline-adoption + the ALTER
chk "rows preserved"                  "$(sqlite3 "$ORG/data.db" 'SELECT count(*) FROM booking;')" "3"
chk "notes column added"              "$(sqlite3 "$ORG/data.db" 'PRAGMA table_info(booking);' | grep -c '|notes|')" "1"
chk "0001+0002 recorded"              "$(sqlite3 "$ORG/data.db" 'SELECT count(*) FROM _schema_migrations;')" "2"

# the backup is a valid, RESTORABLE pre-migration snapshot
BK=$(ls "$ORG"/.backups/*.db 2>/dev/null | head -1)
chk "backup file exists"              "$([ -f "$BK" ] && echo yes)" "yes"
chk "backup retains the data"         "$(sqlite3 "$BK" 'SELECT count(*) FROM booking;')" "3"
chk "backup predates 0002 (no notes)" "$(sqlite3 "$BK" 'PRAGMA table_info(booking);' | grep -c '|notes|')" "0"
chk "backup predates adoption"        "$(sqlite3 "$BK" "SELECT count(*) FROM sqlite_master WHERE name='_schema_migrations';")" "0"

# idempotent re-run
CEL_APPS_DIR="$APPS" "$BIN" migrate app 2>&1 | grep -q "0 applied, 2 already current"
chk "idempotent re-run"               "$?" "0"

echo
[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILED"
exit "$fail"
