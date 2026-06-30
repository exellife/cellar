/* Session strategy adapters (the DB-backed half of session management). These hold
 * the cel_sessions storage + token hashing, so they pull sqlite/sodium/app_db —
 * kept out of session.c so policy/cel_apps can bind a policy without those deps.
 * fixed = absolute expiry (today's behavior); sliding = idle window with lazy
 * renewal + an optional absolute cap. See docs/session-management.md (incl. the
 * future stateless/JWT + external-store adapters that would join this seam). */
#include "session.h"
#include "password.h"     /* cel_random_token_hex, cel_token_hash */
#include "app_db.h"
#include "db_sqlite.h"    /* cel_db_*, cel_now_epoch */

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>

#define TOKEN_BYTES 32                 /* 256-bit -> 64 hex chars (matches auth.c) */

static bool tx(sqlite3 *c, const char *cmd) {
    return sqlite3_exec(c, cmd, NULL, NULL, NULL) == SQLITE_OK;
}

/* Insert a session row (expires_at = now + ttl) + refresh last_login_at. Shared by
 * fixed and sliding — they issue identically; they differ only at verify. */
static int session_issue(const cel_session_policy_t *pol, const char *user_id,
                         char *out_token, size_t token_size) {
    if (cel_random_token_hex(out_token, token_size, TOKEN_BYTES) != 0) return CEL_AUTH_DBERR;
    char thash[65];
    if (cel_token_hash(out_token, thash, sizeof thash) != 0) return CEL_AUTH_DBERR;

    app_db_t *app = app_db_current();
    if (!app) return CEL_AUTH_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    int rc = CEL_AUTH_DBERR;
    if (c) {
        char exp[24], now[24];
        snprintf(exp, sizeof exp, "%ld", cel_now_epoch() + pol->ttl_seconds);
        snprintf(now, sizeof now, "%ld", cel_now_epoch());
        if (tx(c, "BEGIN")) {
            const char *ins[3] = { thash, user_id, exp };
            bool ok = cel_db_exec(c, "INSERT INTO cel_sessions(token, user_id, expires_at) "
                                "VALUES(?1, ?2, ?3)", ins, 3);
            if (ok) {
                const char *up[2] = { now, user_id };
                cel_db_exec(c, "UPDATE cel_users SET last_login_at=?1 WHERE id=?2", up, 2);
            }
            rc = (ok && tx(c, "COMMIT")) ? CEL_AUTH_OK : CEL_AUTH_DBERR;
            if (rc != CEL_AUTH_OK) tx(c, "ROLLBACK");
        }
        app_db_conn_release(app, c);
    }
    app_db_write_unlock(app);
    return rc;
}

/* Resolve token → user, enforcing expiry (expires_at > now) in SQL. Optionally
 * returns created_at/expires_at for the sliding renewal decision. */
static int session_lookup(const char *token, cel_user_t *out, long *created, long *expires) {
    char thash[65];
    if (cel_token_hash(token, thash, sizeof thash) != 0) return CEL_AUTH_INVALID;

    app_db_t *app = app_db_current();
    if (!app) return CEL_AUTH_DBERR;
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) return CEL_AUTH_DBERR;
    int rc = CEL_AUTH_DBERR;

    char nowbuf[24]; snprintf(nowbuf, sizeof nowbuf, "%ld", cel_now_epoch());
    const char *p[2] = { thash, nowbuf };
    sqlite3_stmt *st;
    if (cel_db_prep(c,
        "SELECT u.id, u.email, u.role, (u.email_verified_at IS NOT NULL), s.created_at, s.expires_at "
        "FROM cel_sessions s JOIN cel_users u ON u.id = s.user_id "
        "WHERE s.token=?1 AND s.expires_at > ?2 AND u.is_active=1", p, 2, &st) != SQLITE_OK) {
        app_db_conn_release(app, c); return CEL_AUTH_DBERR;
    }
    int step = sqlite3_step(st);
    if (step == SQLITE_ROW) {
        snprintf(out->id,    sizeof out->id,    "%s", (const char *)sqlite3_column_text(st, 0));
        snprintf(out->email, sizeof out->email, "%s", (const char *)sqlite3_column_text(st, 1));
        snprintf(out->role,  sizeof out->role,  "%s", (const char *)sqlite3_column_text(st, 2));
        out->email_verified = sqlite3_column_int(st, 3) != 0;
        if (created) *created = (long)sqlite3_column_int64(st, 4);
        if (expires) *expires = (long)sqlite3_column_int64(st, 5);
        rc = CEL_AUTH_OK;
    } else if (step == SQLITE_DONE) {
        rc = CEL_AUTH_INVALID;
    }
    sqlite3_finalize(st);
    app_db_conn_release(app, c);
    return rc;
}

/* Best-effort: push a session's deadline out (sliding renewal). Failure is silent —
 * the session is still valid; it just wasn't extended this time. */
static void session_touch(const char *token, long new_expires) {
    char thash[65];
    if (cel_token_hash(token, thash, sizeof thash) != 0) return;
    app_db_t *app = app_db_current();
    if (!app) return;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (c) {
        char e[24]; snprintf(e, sizeof e, "%ld", new_expires);
        const char *p[2] = { e, thash };
        cel_db_exec(c, "UPDATE cel_sessions SET expires_at=?1 WHERE token=?2", p, 2);
        app_db_conn_release(app, c);
    }
    app_db_write_unlock(app);
}

/* ---- adapters ------------------------------------------------------------- */

static int fixed_verify(const cel_session_policy_t *pol, const char *token, cel_user_t *out) {
    (void)pol;   /* absolute expiry already enforced in SQL; no renewal */
    return session_lookup(token, out, NULL, NULL);
}

static int sliding_verify(const cel_session_policy_t *pol, const char *token, cel_user_t *out) {
    long created = 0, expires = 0;
    int rc = session_lookup(token, out, &created, &expires);
    if (rc != CEL_AUTH_OK) return rc;

    long now = cel_now_epoch();
    /* hard absolute cap (independent of the idle window) */
    if (pol->absolute_max_seconds > 0 && created + (long)pol->absolute_max_seconds <= now)
        return CEL_AUTH_INVALID;

    /* lazy renewal: push the deadline once HALF OR LESS of the idle window remains,
     * never beyond the absolute cap. Integer math (no float truncation at the
     * boundary); `<=` so a session polled exactly every ttl/2 still renews rather
     * than expiring on the tick. Bounds writes to ~once per half-window. */
    long idle = pol->ttl_seconds;
    long remaining = expires - now;
    if (idle > 0 && remaining * 2 <= idle) {
        long new_expires = now + idle;
        if (pol->absolute_max_seconds > 0) {
            long cap = created + (long)pol->absolute_max_seconds;
            if (new_expires > cap) new_expires = cap;
        }
        if (new_expires > expires) session_touch(token, new_expires);
    }
    return CEL_AUTH_OK;
}

static const cel_session_strategy_t FIXED   = { session_issue, fixed_verify };
static const cel_session_strategy_t SLIDING = { session_issue, sliding_verify };

const cel_session_strategy_t *cel_session_strategy_for(cel_session_strategy_e s) {
    switch (s) {
        case CEL_SESSION_FIXED:   return &FIXED;
        case CEL_SESSION_SLIDING: return &SLIDING;
        default:                  return NULL;
    }
}
