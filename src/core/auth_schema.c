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

    /* Email verification is a 6-digit CODE (not a link), verified against the
     * authenticated session — one pending code per user (user_id PK), stored as
     * sha256(code) with a short TTL, an attempt cap, and a resend cooldown. */
    "CREATE TABLE IF NOT EXISTS cel_email_verifications ("
    "  user_id      TEXT PRIMARY KEY REFERENCES cel_users(id) ON DELETE CASCADE,"
    "  code_hash    TEXT NOT NULL,"
    "  expires_at   INTEGER NOT NULL,"
    "  attempts     INTEGER NOT NULL DEFAULT 0,"
    "  last_sent_at INTEGER NOT NULL DEFAULT 0,"
    "  sends        INTEGER NOT NULL DEFAULT 0,"
    "  created_at   INTEGER NOT NULL DEFAULT (unixepoch())"
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
#define CEL_AUTH_SCHEMA_VERSION 6

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

/* v4 -> v5: email verification moves from a random link TOKEN to a 6-digit CODE
 * verified against the authenticated session. The table is re-keyed (token PK ->
 * user_id PK) and gains code_hash/attempts/last_sent_at, so an existing table is
 * dropped and recreated (any pending link verifications are discarded — the user
 * just requests a fresh code). A fresh db already has the new shape from the base
 * schema, so this only runs on an already-provisioned db (gated v >= 1). */
static const char *AUTH_SCHEMA_V5 =
    "DROP TABLE IF EXISTS cel_email_verifications;"
    "CREATE TABLE IF NOT EXISTS cel_email_verifications ("
    "  user_id      TEXT PRIMARY KEY REFERENCES cel_users(id) ON DELETE CASCADE,"
    "  code_hash    TEXT NOT NULL,"
    "  expires_at   INTEGER NOT NULL,"
    "  attempts     INTEGER NOT NULL DEFAULT 0,"
    "  last_sent_at INTEGER NOT NULL DEFAULT 0,"
    "  sends        INTEGER NOT NULL DEFAULT 0,"
    "  created_at   INTEGER NOT NULL DEFAULT (unixepoch())"
    ");";

/* v5 -> v6: cel_email_verifications gains a per-cycle `sends` counter (email-abuse
 * cap). A fresh db and any v4->v5 db already have the column (base schema + V5's
 * CREATE both carry it), so this ALTER runs ONLY for a db that stopped exactly at
 * v5 — gated `v == 5` below (ADD COLUMN is not idempotent). */
static const char *AUTH_SCHEMA_V6 =
    "ALTER TABLE cel_email_verifications ADD COLUMN sends INTEGER NOT NULL DEFAULT 0;";

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

    /* Run the whole migration (steps + the user_version stamp) ATOMICALLY: DDL and
     * PRAGMA user_version are both transactional in SQLite, so a crash mid-migration
     * rolls back cleanly and the next open re-runs from the correct prior version —
     * no half-applied schema, no non-idempotent step re-running against a partial db. */
    if (exec_or_log(db, "BEGIN IMMEDIATE") != 0) return -1;

    int rc = 0;
    do {
        if (v < 1 && (rc = exec_or_log(db, AUTH_SCHEMA)) != 0) break;
        /* Only an existing v1 db needs the ALTERs (a fresh db got the columns from the
         * base schema above, so applying them again would error on a duplicate column). */
        if (v == 1 && (rc = exec_or_log(db, AUTH_SCHEMA_V2)) != 0) break;
        /* v < 3 adds cel_device_tokens to an already-provisioned db (IF NOT EXISTS). */
        if (v < 3 && (rc = exec_or_log(db, AUTH_SCHEMA_V3)) != 0) break;
        /* v < 4 adds cel_push_subscriptions (same idempotent IF NOT EXISTS pattern). */
        if (v < 4 && (rc = exec_or_log(db, AUTH_SCHEMA_V4)) != 0) break;
        /* v1..v4 -> v5 re-keys cel_email_verifications (token -> 6-digit code). DROP +
         * CREATE IF NOT EXISTS, gated v >= 1 (a fresh db already got it from base). */
        if (v >= 1 && v < 5 && (rc = exec_or_log(db, AUTH_SCHEMA_V5)) != 0) break;
        /* v5 -> v6 adds the `sends` column; ONLY a db stopped exactly at v5 lacks it
         * (fresh + v4->v5 already have it from base/V5), so gate on v == 5. */
        if (v == 5 && (rc = exec_or_log(db, AUTH_SCHEMA_V6)) != 0) break;

        char stamp[48];
        snprintf(stamp, sizeof stamp, "PRAGMA user_version = %d", CEL_AUTH_SCHEMA_VERSION);
        rc = exec_or_log(db, stamp);
    } while (0);

    if (rc != 0) { exec_or_log(db, "ROLLBACK"); return -1; }
    return exec_or_log(db, "COMMIT");
}
