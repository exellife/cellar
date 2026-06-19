#include "auth_schema.h"
#include "logger.h"

#include <sqlite3.h>
#include <stdio.h>

/* Regular (non-STRICT) tables: INTEGER-affinity columns coerce the text the auth
 * layer binds ("1718…") into integers losslessly, so the binder stays uniform.
 * Timestamps are unix-epoch seconds; booleans are 0/1; ids are TEXT uuids. */
static const char *AUTH_SCHEMA =
    "CREATE TABLE IF NOT EXISTS cel_users ("
    "  id                   TEXT PRIMARY KEY,"
    "  email                TEXT NOT NULL UNIQUE,"
    "  role                 TEXT NOT NULL DEFAULT 'viewer',"
    "  is_active            INTEGER NOT NULL DEFAULT 1,"
    "  email_verified_at    INTEGER,"
    "  failed_login_count   INTEGER NOT NULL DEFAULT 0,"
    "  last_failed_login_at INTEGER,"
    "  locked_until         INTEGER,"
    "  created_at           INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  last_login_at        INTEGER"
    ");"

    "CREATE TABLE IF NOT EXISTS cel_identities ("
    "  id           TEXT PRIMARY KEY,"
    "  user_id      TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,"
    "  provider     TEXT NOT NULL,"
    "  provider_uid TEXT NOT NULL,"
    "  secret       TEXT,"
    "  created_at   INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  UNIQUE(provider, provider_uid)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_cel_identities_user ON cel_identities(user_id);"

    "CREATE TABLE IF NOT EXISTS cel_sessions ("
    "  token      TEXT PRIMARY KEY,"
    "  user_id    TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  expires_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_cel_sessions_user    ON cel_sessions(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_cel_sessions_expires ON cel_sessions(expires_at);"

    "CREATE TABLE IF NOT EXISTS cel_password_resets ("
    "  token      TEXT PRIMARY KEY,"
    "  user_id    TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,"
    "  expires_at INTEGER NOT NULL,"
    "  used_at    INTEGER,"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch())"
    ");"

    "CREATE TABLE IF NOT EXISTS cel_email_verifications ("
    "  token      TEXT PRIMARY KEY,"
    "  user_id    TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,"
    "  expires_at INTEGER NOT NULL,"
    "  used_at    INTEGER,"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch())"
    ");"

    /* MFA tables — created now so auth's cross-references (e.g. dropping pending
     * challenges on password reset) work; mfa.c moves onto them next. */
    "CREATE TABLE IF NOT EXISTS cel_mfa ("
    "  user_id         TEXT PRIMARY KEY REFERENCES cel_users(id) ON DELETE CASCADE,"
    "  type            TEXT NOT NULL DEFAULT 'totp',"
    "  secret          TEXT NOT NULL,"
    "  confirmed_at    INTEGER,"
    "  failed_attempts INTEGER NOT NULL DEFAULT 0,"  /* per-user MFA-verify failures (M-1) */
    "  locked_until    INTEGER,"                     /* MFA verification locked until (epoch) */
    "  created_at      INTEGER NOT NULL DEFAULT (unixepoch())"
    ");"

    "CREATE TABLE IF NOT EXISTS cel_mfa_challenges ("
    "  token      TEXT PRIMARY KEY,"
    "  user_id    TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,"
    "  attempts   INTEGER NOT NULL DEFAULT 0,"
    "  expires_at INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch())"
    ");"

    "CREATE TABLE IF NOT EXISTS cel_mfa_recovery ("
    "  id         TEXT PRIMARY KEY,"
    "  user_id    TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,"
    "  code_hash  TEXT NOT NULL,"
    "  used_at    INTEGER,"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch())"
    ");";

/* Bump this when the cel_* infra schema changes, and add the matching ALTER step
 * in cel_auth_schema_apply below. Tracked per app via SQLite's PRAGMA
 * user_version, so a newer cellar can evolve an existing app's bundle in place. */
#define CEL_AUTH_SCHEMA_VERSION 2

/* v1 -> v2: per-user MFA-verify lockout (M-1). A fresh db gets these via the base
 * AUTH_SCHEMA above; only a db already at v1 needs the ALTERs. */
static const char *AUTH_SCHEMA_V2 =
    "ALTER TABLE cel_mfa ADD COLUMN failed_attempts INTEGER NOT NULL DEFAULT 0;"
    "ALTER TABLE cel_mfa ADD COLUMN locked_until INTEGER;";

static long user_version(struct sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    long v = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        v = (long)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static int exec_or_log(struct sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOG_ERROR("auth schema: %s", err ? err : "unknown error");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int cel_auth_schema_apply(struct sqlite3 *db) {
    long v = user_version(db);
    if (v >= CEL_AUTH_SCHEMA_VERSION) return 0;   /* already current */

    if (v < 1 && exec_or_log(db, AUTH_SCHEMA) != 0) return -1;
    /* Only an existing v1 db needs the ALTERs (a fresh db got the columns from the
     * base schema above, so applying them again would error on a duplicate column). */
    if (v == 1 && exec_or_log(db, AUTH_SCHEMA_V2) != 0) return -1;

    char stamp[48];
    snprintf(stamp, sizeof stamp, "PRAGMA user_version = %d", CEL_AUTH_SCHEMA_VERSION);
    return exec_or_log(db, stamp);
}
