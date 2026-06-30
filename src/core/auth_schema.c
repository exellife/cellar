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

    /* Long-lived, revocable device tokens (PIN fast-login): a client stores one
     * (encrypted, e.g. behind a PIN) and exchanges it for a fresh session without
     * re-entering the password. `token` is the sha-256 hash (never raw at rest);
     * `id` is the public handle for list/revoke. Opt-in per app via
     * _session.device_ttl_seconds. */
    "CREATE TABLE IF NOT EXISTS cel_device_tokens ("
    "  token        TEXT PRIMARY KEY,"
    "  id           TEXT NOT NULL UNIQUE,"
    "  user_id      TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,"
    "  label        TEXT,"
    "  created_at   INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  last_used_at INTEGER,"
    "  expires_at   INTEGER NOT NULL,"
    "  revoked_at   INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_cel_device_tokens_user ON cel_device_tokens(user_id);"

    /* Web-Push subscriptions (NotifChannel off-site delivery): one row per browser/
     * device a user enabled push on. endpoint is the push-service URL (dedup key);
     * p256dh/auth are the client keys for payload encryption (slice 2). disabled_at
     * is set when the push service reports the subscription gone (404/410). */
    "CREATE TABLE IF NOT EXISTS cel_push_subscriptions ("
    "  id           TEXT PRIMARY KEY,"
    "  user_id      TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,"
    "  endpoint     TEXT NOT NULL UNIQUE,"
    "  p256dh       TEXT NOT NULL,"
    "  auth         TEXT NOT NULL,"
    "  ua           TEXT,"
    "  created_at   INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  last_used_at INTEGER,"
    "  disabled_at  INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_cel_push_subscriptions_user ON cel_push_subscriptions(user_id);"

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
#define CEL_AUTH_SCHEMA_VERSION 4

/* v1 -> v2: per-user MFA-verify lockout (M-1). A fresh db gets these via the base
 * AUTH_SCHEMA above; only a db already at v1 needs the ALTERs. */
static const char *AUTH_SCHEMA_V2 =
    "ALTER TABLE cel_mfa ADD COLUMN failed_attempts INTEGER NOT NULL DEFAULT 0;"
    "ALTER TABLE cel_mfa ADD COLUMN locked_until INTEGER;";

/* v2 -> v3: device tokens (PIN fast-login). The table is also in the base AUTH_SCHEMA
 * (a fresh db gets it there); this step adds it to an ALREADY-PROVISIONED db, which
 * the version gate would otherwise skip. CREATE ... IF NOT EXISTS, so it's safe to
 * run on a fresh db too (unlike the V2 ADD COLUMNs). */
static const char *AUTH_SCHEMA_V3 =
    "CREATE TABLE IF NOT EXISTS cel_device_tokens ("
    "  token        TEXT PRIMARY KEY,"
    "  id           TEXT NOT NULL UNIQUE,"
    "  user_id      TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,"
    "  label        TEXT,"
    "  created_at   INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  last_used_at INTEGER,"
    "  expires_at   INTEGER NOT NULL,"
    "  revoked_at   INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_cel_device_tokens_user ON cel_device_tokens(user_id);";

/* v3 -> v4: web-push subscriptions (NotifChannel). Same pattern as V3 — also in the
 * base schema for fresh dbs; this adds it to an already-provisioned db. IF NOT EXISTS. */
static const char *AUTH_SCHEMA_V4 =
    "CREATE TABLE IF NOT EXISTS cel_push_subscriptions ("
    "  id           TEXT PRIMARY KEY,"
    "  user_id      TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,"
    "  endpoint     TEXT NOT NULL UNIQUE,"
    "  p256dh       TEXT NOT NULL,"
    "  auth         TEXT NOT NULL,"
    "  ua           TEXT,"
    "  created_at   INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  last_used_at INTEGER,"
    "  disabled_at  INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_cel_push_subscriptions_user ON cel_push_subscriptions(user_id);";

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
    /* v < 3 adds cel_device_tokens to an already-provisioned db. IF NOT EXISTS, so
     * harmless on a fresh db that just got it from the base schema. */
    if (v < 3 && exec_or_log(db, AUTH_SCHEMA_V3) != 0) return -1;
    /* v < 4 adds cel_push_subscriptions (same idempotent IF NOT EXISTS pattern). */
    if (v < 4 && exec_or_log(db, AUTH_SCHEMA_V4) != 0) return -1;

    char stamp[48];
    snprintf(stamp, sizeof stamp, "PRAGMA user_version = %d", CEL_AUTH_SCHEMA_VERSION);
    return exec_or_log(db, stamp);
}
