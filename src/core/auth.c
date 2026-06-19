#include "auth.h"
#include "password.h"
#include "app_db.h"
#include "db_sqlite.h"
#include "session_cache.h"
#include "mfa.h"
#include "metrics.h"
#include "logger.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TOKEN_BYTES 32   /* 256-bit -> 64 hex chars */

/* A decoy password hash, verified against when an email is not found, so that a
 * missing user takes the same time as a wrong password (no enumeration oracle).
 * Computed once at startup via cel_auth_init(). */
static char g_decoy_hash[256];

int cel_auth_init(void) {
    if (cel_password_hash("cellar-decoy-password-never-matches", g_decoy_hash,
                          sizeof g_decoy_hash) != 0) {
        g_decoy_hash[0] = '\0';
        return -1;   /* without the decoy, login timing distinguishes unknown users (L-1) */
    }
    return 0;
}

/* ---- SQLite helpers -------------------------------------------------------- */

/* Transaction control on `c`; true on success. Multi-statement credential writes
 * run in a transaction (under the per-app write lock) to stay atomic. */
static bool tx(sqlite3 *c, const char *cmd) {
    return sqlite3_exec(c, cmd, NULL, NULL, NULL) == SQLITE_OK;
}

/* ---- login lockout --------------------------------------------------------- */

/* Per-account login lockout config (0 = disabled, the default). */
static int g_lockout_n = 0;   /* failures before lock */
static int g_lockout_w = 0;   /* streak window AND lock duration (seconds) */

void cel_auth_set_lockout(int limit, int window_seconds) {
    g_lockout_n = (limit > 0 && window_seconds > 0) ? limit : 0;
    g_lockout_w = (limit > 0 && window_seconds > 0) ? window_seconds : 0;
}

/* Record one failed login on connection `c`: extend the streak (or start fresh
 * if the last failure was outside the window), and lock the account once it hits
 * the limit. now/locked_until are computed in C (SQLite has no now()). */
static void lockout_record_failure(sqlite3 *c, const char *user_id, int failed, long last_failed) {
    long now = cel_now_epoch();
    int newcount = (last_failed == 0 || (now - last_failed) > g_lockout_w) ? 1 : failed + 1;
    char cnt[16], nowbuf[24], locked[24];
    snprintf(cnt, sizeof cnt, "%d", newcount);
    snprintf(nowbuf, sizeof nowbuf, "%ld", now);
    bool lock = newcount >= g_lockout_n;
    if (lock) snprintf(locked, sizeof locked, "%ld", now + g_lockout_w);
    const char *p[4] = { cnt, nowbuf, lock ? locked : NULL, user_id };
    cel_db_exec(c, "UPDATE cel_users SET failed_login_count=?1, last_failed_login_at=?2, "
              "locked_until=?3 WHERE id=?4", p, 4);
}

static void lockout_reset(sqlite3 *c, const char *user_id) {
    const char *p[1] = { user_id };
    cel_db_exec(c, "UPDATE cel_users SET failed_login_count=0, last_failed_login_at=NULL, "
              "locked_until=NULL WHERE id=?1", p, 1);
}

int cel_auth_unlock(const char *email) {
    if (!email || !*email) return -1;
    app_db_t *app = app_db_current();
    if (!app) return -1;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    int n = -1;
    if (c) {
        const char *p[1] = { email };
        if (cel_db_exec(c, "UPDATE cel_users SET failed_login_count=0, last_failed_login_at=NULL, "
                      "locked_until=NULL WHERE email=?1", p, 1))
            n = sqlite3_changes(c);
        app_db_conn_release(app, c);
    }
    app_db_write_unlock(app);
    return n;
}

/* ---- login ----------------------------------------------------------------- */

int cel_auth_login(const char *email, const char *password, int ttl_seconds,
                   char *out_token, size_t token_size,
                   char *out_challenge, size_t challenge_size,
                   cel_user_t *out_user) {
    app_db_t *app = app_db_current();
    if (!app) return CEL_AUTH_DBERR;
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) return CEL_AUTH_DBERR;
    int rc = CEL_AUTH_DBERR;

    /* Resolve the password IDENTITY (provider_uid = email) and its user. Same
     * shape a federated login uses (provider='google', uid=the 'sub'). */
    char uid[37] = {0}, hash[256] = {0}, role[32] = {0}, uemail[256] = {0};
    bool active = false, email_verified = false, locked = false;
    int failed = 0; long last_failed = 0;
    bool found = false;
    {
        const char *p[1] = { email };
        sqlite3_stmt *st;
        if (cel_db_prep(c,
            "SELECT u.id, i.secret, u.role, u.is_active, u.email, "
            "(u.email_verified_at IS NOT NULL), u.locked_until, "
            "u.failed_login_count, COALESCE(u.last_failed_login_at, 0) "
            "FROM cel_identities i JOIN cel_users u ON u.id = i.user_id "
            "WHERE i.provider='password' AND i.provider_uid=?1", p, 1, &st) != SQLITE_OK) {
            app_db_conn_release(app, c); return CEL_AUTH_DBERR;
        }
        int step = sqlite3_step(st);
        if (step == SQLITE_ROW) {
            found = true;
            snprintf(uid,    sizeof uid,    "%s", (const char *)sqlite3_column_text(st, 0));
            if (sqlite3_column_type(st, 1) != SQLITE_NULL)
                snprintf(hash, sizeof hash, "%s", (const char *)sqlite3_column_text(st, 1));
            snprintf(role,   sizeof role,   "%s", (const char *)sqlite3_column_text(st, 2));
            active = sqlite3_column_int(st, 3) != 0;
            snprintf(uemail, sizeof uemail, "%s", (const char *)sqlite3_column_text(st, 4));
            email_verified = sqlite3_column_int(st, 5) != 0;
            if (sqlite3_column_type(st, 6) != SQLITE_NULL)
                locked = sqlite3_column_int64(st, 6) > cel_now_epoch();
            failed      = sqlite3_column_int(st, 7);
            last_failed = (long)sqlite3_column_int64(st, 8);
        }
        sqlite3_finalize(st);
    }
    app_db_conn_release(app, c);

    if (!found) {
        if (g_decoy_hash[0]) cel_password_verify(g_decoy_hash, password);  /* equalize timing */
        rc = CEL_AUTH_INVALID;
        goto out;
    }

    /* Always verify (even when inactive or the secret is absent) so timing
     * doesn't distinguish the cases — a null secret falls back to the decoy. */
    bool ok;
    if (hash[0]) {
        ok = cel_password_verify(hash, password);
    } else {
        if (g_decoy_hash[0]) cel_password_verify(g_decoy_hash, password);
        ok = false;
    }

    if (g_lockout_n > 0) {
        if (locked) { rc = CEL_AUTH_LOCKED; goto out; }   /* locked even if pw is right */
        if (!active || !ok) {
            app_db_write_lock(app);
            sqlite3 *wc = app_db_conn_acquire(app);
            if (wc) { lockout_record_failure(wc, uid, failed, last_failed); app_db_conn_release(app, wc); }
            app_db_write_unlock(app);
            rc = CEL_AUTH_INVALID;
            goto out;
        }
        if (failed > 0) {                                 /* clear the streak on success */
            app_db_write_lock(app);
            sqlite3 *wc = app_db_conn_acquire(app);
            if (wc) { lockout_reset(wc, uid); app_db_conn_release(app, wc); }
            app_db_write_unlock(app);
        }
    } else if (!active || !ok) {
        rc = CEL_AUTH_INVALID;
        goto out;
    }

    snprintf(out_user->id,    sizeof out_user->id,    "%s", uid);
    snprintf(out_user->email, sizeof out_user->email, "%s", uemail);
    snprintf(out_user->role,  sizeof out_user->role,  "%s", role);
    out_user->tenant_id[0] = '\0';
    out_user->email_verified = email_verified;

    /* Second factor? A confirmed TOTP enrollment (with MFA enabled) means we issue
     * a one-time challenge instead of a session; the caller completes the login via
     * cel_mfa_verify_login. Otherwise mint the session immediately. */
    if (cel_mfa_required_for(out_user->id)) {
        rc = (cel_mfa_create_challenge(out_user->id, out_challenge, challenge_size) == 0)
                 ? CEL_AUTH_MFA_REQUIRED : CEL_AUTH_DBERR;
        goto out;
    }
    rc = cel_auth_issue_session(out_user->id, ttl_seconds, out_token, token_size);
out:
    if      (rc == CEL_AUTH_OK)                                  cel_metric_inc(CEL_M_LOGIN_OK);
    else if (rc == CEL_AUTH_INVALID || rc == CEL_AUTH_LOCKED)    cel_metric_inc(CEL_M_LOGIN_FAIL);
    return rc;
}

/* Mint a session for an already-authenticated user (shared by the password path,
 * MFA verify, and federated login). Token stored hashed; raw returned. */
int cel_auth_issue_session(const char *user_id, int ttl_seconds,
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
        snprintf(exp, sizeof exp, "%ld", cel_now_epoch() + ttl_seconds);
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

int cel_auth_oauth_login(const char *provider, const char *sub,
                         const char *email, bool email_verified, bool email_link_trusted,
                         const char *provision_role, int ttl_seconds,
                         char *out_token, size_t token_size,
                         char *out_challenge, size_t challenge_size, cel_user_t *out_user) {
    if (!provider || !*provider || !sub || !*sub) return CEL_AUTH_INVALID;
    app_db_t *app = app_db_current();
    if (!app) return CEL_AUTH_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) { app_db_write_unlock(app); return CEL_AUTH_DBERR; }
    int rc = CEL_AUTH_DBERR;
    char user_id[37] = {0};

    /* 1. an existing federated identity resolves straight to its user. */
    {
        const char *p[2] = { provider, sub };
        int f = cel_db_one_text(c, "SELECT user_id FROM cel_identities WHERE provider=?1 AND provider_uid=?2",
                           p, 2, user_id, sizeof user_id);
        if (f < 0) goto out;
    }

    /* 2. an account may already exist for this verified email. Auto-LINK ONLY when
     *    the provider is explicitly trusted for the email's domain (H-3) — never on
     *    the provider's email_verified claim alone. An untrusted collision is
     *    REFUSED, not silently merged. */
    if (!user_id[0] && email_verified && email && *email) {
        char existing[37] = {0};
        const char *p[1] = { email };
        int f = cel_db_one_text(c, "SELECT id FROM cel_users WHERE email=?1", p, 1, existing, sizeof existing);
        if (f < 0) goto out;
        if (f == 1) {
            if (!email_link_trusted) { rc = CEL_AUTH_INVALID; goto out; }  /* refuse silent merge */
            snprintf(user_id, sizeof user_id, "%s", existing);
            char iid[37]; cel_uuid_v4(iid, sizeof iid);
            const char *ins[4] = { iid, user_id, provider, sub };
            cel_db_exec(c, "INSERT INTO cel_identities(id, user_id, provider, provider_uid) "
                      "VALUES(?1,?2,?3,?4) ON CONFLICT(provider, provider_uid) DO NOTHING", ins, 4);
        }
    }

    /* 3. otherwise auto-provision — only if signup is allowed (a role) and we have
     *    an email. No account + can't provision => INVALID. */
    if (!user_id[0]) {
        if (!provision_role || !*provision_role || !email || !*email) { rc = CEL_AUTH_INVALID; goto out; }
        if (!tx(c, "BEGIN")) goto out;
        char nid[37]; cel_uuid_v4(nid, sizeof nid);
        const char *up[3] = { nid, email, provision_role };
        bool ok = cel_db_exec(c, "INSERT INTO cel_users(id, email, role) VALUES(?1,?2,?3)", up, 3);
        if (ok) {
            snprintf(user_id, sizeof user_id, "%s", nid);
            char iid[37]; cel_uuid_v4(iid, sizeof iid);
            const char *ins[4] = { iid, user_id, provider, sub };
            ok = cel_db_exec(c, "INSERT INTO cel_identities(id, user_id, provider, provider_uid) "
                           "VALUES(?1,?2,?3,?4)", ins, 4);
        }
        if (!ok || !tx(c, "COMMIT")) { tx(c, "ROLLBACK"); goto out; }   /* rc stays DBERR */
    }

    /* A provider-verified email confirms the account's email only when trusted. */
    if (email_verified && email && *email && email_link_trusted) {
        char now[24]; snprintf(now, sizeof now, "%ld", cel_now_epoch());
        const char *uv[3] = { now, user_id, email };
        cel_db_exec(c, "UPDATE cel_users SET email_verified_at=COALESCE(email_verified_at, ?1) "
                  "WHERE id=?2 AND email=?3", uv, 3);
    }

    app_db_conn_release(app, c);
    app_db_write_unlock(app);
    app = NULL;

    /* Second factor (H-2): federated login honors the SAME MFA gate as the
     * password path — a confirmed TOTP enrollment issues a challenge, not a
     * session; the client finishes via cel_mfa_verify_login. */
    if (out_challenge && challenge_size && cel_mfa_required_for(user_id)) {
        return (cel_mfa_create_challenge(user_id, out_challenge, challenge_size) == 0)
                   ? CEL_AUTH_MFA_REQUIRED : CEL_AUTH_DBERR;
    }
    rc = cel_auth_issue_session(user_id, ttl_seconds, out_token, token_size);
    if (rc == CEL_AUTH_OK) {
        if (cel_auth_verify(out_token, out_user) != CEL_AUTH_OK) rc = CEL_AUTH_DBERR;
        else cel_metric_inc(CEL_M_LOGIN_OK);
    }
    return rc;
out:
    app_db_conn_release(app, c);
    app_db_write_unlock(app);
    return rc;
}

int cel_auth_register(const char *email, const char *password, const char *role,
                      int ttl_seconds, char *out_token, size_t token_size,
                      cel_user_t *out_user) {
    char hash[256];
    if (cel_password_hash(password, hash, sizeof hash) != 0) return CEL_AUTH_DBERR;
    char thash[65];
    if (cel_random_token_hex(out_token, token_size, TOKEN_BYTES) != 0) return CEL_AUTH_DBERR;
    if (cel_token_hash(out_token, thash, sizeof thash) != 0) return CEL_AUTH_DBERR;

    app_db_t *app = app_db_current();
    if (!app) return CEL_AUTH_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) { app_db_write_unlock(app); return CEL_AUTH_DBERR; }
    int rc = CEL_AUTH_DBERR;
    bool in_txn = false;

    /* Account + password identity + session are written atomically. A duplicate
     * email must fail (not silently take over an existing account). */
    if (!tx(c, "BEGIN")) goto out;
    in_txn = true;

    char uid[37]; cel_uuid_v4(uid, sizeof uid);

    /* 1. the account row. */
    {
        const char *p[3] = { uid, email, role };
        if (!cel_db_exec(c, "INSERT INTO cel_users(id, email, role) VALUES(?1, ?2, ?3)", p, 3)) {
            if (sqlite3_extended_errcode(c) == SQLITE_CONSTRAINT_UNIQUE) rc = CEL_AUTH_CONFLICT;
            else LOG_ERROR("register failed: %s", sqlite3_errmsg(c));
            goto out;
        }
        snprintf(out_user->id,    sizeof out_user->id,    "%s", uid);
        snprintf(out_user->email, sizeof out_user->email, "%s", email);
        snprintf(out_user->role,  sizeof out_user->role,  "%s", role);
        out_user->tenant_id[0] = '\0';
        out_user->email_verified = false;
    }

    /* 2. the password identity (provider='password', uid=email). */
    {
        char iid[37]; cel_uuid_v4(iid, sizeof iid);
        const char *p[4] = { iid, uid, email, hash };
        if (!cel_db_exec(c, "INSERT INTO cel_identities(id, user_id, provider, provider_uid, secret) "
                       "VALUES(?1, ?2, 'password', ?3, ?4)", p, 4)) {
            if (sqlite3_extended_errcode(c) == SQLITE_CONSTRAINT_UNIQUE) rc = CEL_AUTH_CONFLICT;
            else LOG_ERROR("register identity failed: %s", sqlite3_errmsg(c));
            goto out;
        }
    }

    /* 3. the auto-login session (token hash stored; raw token returned). */
    {
        char exp[24]; snprintf(exp, sizeof exp, "%ld", cel_now_epoch() + ttl_seconds);
        const char *ins[3] = { thash, uid, exp };
        if (!cel_db_exec(c, "INSERT INTO cel_sessions(token, user_id, expires_at) "
                       "VALUES(?1, ?2, ?3)", ins, 3)) goto out;
    }

    if (!tx(c, "COMMIT")) goto out;
    in_txn = false;
    rc = CEL_AUTH_OK;
out:
    if (in_txn) tx(c, "ROLLBACK");
    app_db_conn_release(app, c);
    app_db_write_unlock(app);
    return rc;
}

int cel_auth_create_user(const char *email, const char *password, const char *role,
                         const char *tenant_id, char *out_id, size_t out_id_size) {
    (void)tenant_id;   /* isolation is the app's file boundary — no tenant column */
    char hash[256];
    if (cel_password_hash(password, hash, sizeof hash) != 0) return CEL_AUTH_DBERR;

    app_db_t *app = app_db_current();
    if (!app) return CEL_AUTH_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) { app_db_write_unlock(app); return CEL_AUTH_DBERR; }
    int rc = CEL_AUTH_DBERR;
    bool in_txn = false;
    char uid[37]; cel_uuid_v4(uid, sizeof uid);

    if (!tx(c, "BEGIN")) goto out;
    in_txn = true;

    {
        const char *p[3] = { uid, email, role };
        if (!cel_db_exec(c, "INSERT INTO cel_users(id, email, role) VALUES(?1, ?2, ?3)", p, 3)) {
            if (sqlite3_extended_errcode(c) == SQLITE_CONSTRAINT_UNIQUE) rc = CEL_AUTH_CONFLICT;
            else LOG_ERROR("create user failed: %s", sqlite3_errmsg(c));
            goto out;
        }
    }
    {
        char iid[37]; cel_uuid_v4(iid, sizeof iid);
        const char *p[4] = { iid, uid, email, hash };
        if (!cel_db_exec(c, "INSERT INTO cel_identities(id, user_id, provider, provider_uid, secret) "
                       "VALUES(?1, ?2, 'password', ?3, ?4)", p, 4)) {
            if (sqlite3_extended_errcode(c) == SQLITE_CONSTRAINT_UNIQUE) rc = CEL_AUTH_CONFLICT;
            else LOG_ERROR("create identity failed: %s", sqlite3_errmsg(c));
            goto out;
        }
    }

    if (!tx(c, "COMMIT")) goto out;
    in_txn = false;
    snprintf(out_id, out_id_size, "%s", uid);
    rc = CEL_AUTH_OK;
out:
    if (in_txn) tx(c, "ROLLBACK");
    app_db_conn_release(app, c);
    app_db_write_unlock(app);
    return rc;
}

int cel_auth_verify(const char *token, cel_user_t *out_user) {
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
        "SELECT u.id, u.email, u.role, (u.email_verified_at IS NOT NULL) "
        "FROM cel_sessions s JOIN cel_users u ON u.id = s.user_id "
        "WHERE s.token=?1 AND s.expires_at > ?2 AND u.is_active=1", p, 2, &st) != SQLITE_OK) {
        app_db_conn_release(app, c); return CEL_AUTH_DBERR;
    }
    int step = sqlite3_step(st);
    if (step == SQLITE_ROW) {
        snprintf(out_user->id,    sizeof out_user->id,    "%s", (const char *)sqlite3_column_text(st, 0));
        snprintf(out_user->email, sizeof out_user->email, "%s", (const char *)sqlite3_column_text(st, 1));
        snprintf(out_user->role,  sizeof out_user->role,  "%s", (const char *)sqlite3_column_text(st, 2));
        out_user->tenant_id[0] = '\0';
        out_user->email_verified = sqlite3_column_int(st, 3) != 0;
        rc = CEL_AUTH_OK;
    } else if (step == SQLITE_DONE) {
        rc = CEL_AUTH_INVALID;
    }
    sqlite3_finalize(st);
    app_db_conn_release(app, c);
    return rc;
}

int cel_auth_resolve(const char *token, cel_user_t *out_user) {
    if (cel_session_cache_get(token, out_user)) return CEL_AUTH_OK;   /* skip the DB */
    int rc = cel_auth_verify(token, out_user);
    if (rc == CEL_AUTH_OK) cel_session_cache_put(token, out_user);
    return rc;
}

int cel_auth_logout(const char *token) {
    cel_session_cache_evict(token);   /* revoke immediately on this instance */
    char thash[65];
    if (cel_token_hash(token, thash, sizeof thash) != 0) return CEL_AUTH_INVALID;
    app_db_t *app = app_db_current();
    if (!app) return CEL_AUTH_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    int rc = CEL_AUTH_DBERR;
    if (c) {
        const char *p[1] = { thash };
        rc = cel_db_exec(c, "DELETE FROM cel_sessions WHERE token=?1", p, 1) ? CEL_AUTH_OK : CEL_AUTH_DBERR;
        app_db_conn_release(app, c);
    }
    app_db_write_unlock(app);
    return rc;
}

int cel_auth_revoke_user_sessions(const char *email) {
    if (!email || !*email) return -1;
    app_db_t *app = app_db_current();
    if (!app) return -1;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    int n = -1;
    if (c) {
        const char *p[1] = { email };
        if (cel_db_exec(c, "DELETE FROM cel_sessions WHERE user_id = "
                      "(SELECT id FROM cel_users WHERE email=?1)", p, 1))
            n = sqlite3_changes(c);
        app_db_conn_release(app, c);
    }
    app_db_write_unlock(app);
    /* The cache can't evict by user, so clear it (same-instance; other instances
     * re-validate within the cache TTL). */
    if (n >= 0) cel_session_cache_clear();
    return n;
}

#define RESET_TTL_SECONDS 3600   /* a password-reset link is good for 1 hour */

int cel_auth_create_password_reset(const char *email, char *out_token, size_t token_size) {
    app_db_t *app = app_db_current();
    if (!app) return CEL_AUTH_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) { app_db_write_unlock(app); return CEL_AUTH_DBERR; }
    int rc = CEL_AUTH_DBERR;

    /* Only an account that actually has a password identity can reset a password. */
    char user_id[37] = {0};
    {
        const char *p[1] = { email };
        int f = cel_db_one_text(c,
            "SELECT u.id FROM cel_identities i JOIN cel_users u ON u.id = i.user_id "
            "WHERE i.provider='password' AND i.provider_uid=?1", p, 1, user_id, sizeof user_id);
        if (f < 0) goto out;
        if (f == 0) { rc = CEL_AUTH_INVALID; goto out; }   /* no account — caller still 200 */
    }

    if (cel_random_token_hex(out_token, token_size, TOKEN_BYTES) != 0) goto out;
    {
        char h[65];
        if (cel_token_hash(out_token, h, sizeof h) != 0) goto out;
        char exp[24]; snprintf(exp, sizeof exp, "%ld", cel_now_epoch() + RESET_TTL_SECONDS);
        const char *ins[3] = { h, user_id, exp };
        rc = cel_db_exec(c, "INSERT INTO cel_password_resets(token, user_id, expires_at) "
                       "VALUES(?1, ?2, ?3)", ins, 3) ? CEL_AUTH_OK : CEL_AUTH_DBERR;
    }
out:
    app_db_conn_release(app, c);
    app_db_write_unlock(app);
    return rc;
}

int cel_auth_perform_password_reset(const char *token, const char *new_password) {
    char hash[256];
    if (cel_password_hash(new_password, hash, sizeof hash) != 0) return CEL_AUTH_DBERR;
    char h[65];
    if (cel_token_hash(token, h, sizeof h) != 0) return CEL_AUTH_INVALID;

    app_db_t *app = app_db_current();
    if (!app) return CEL_AUTH_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) { app_db_write_unlock(app); return CEL_AUTH_DBERR; }
    int rc = CEL_AUTH_DBERR;
    bool in_txn = false;
    char user_id[37] = {0};

    if (!tx(c, "BEGIN")) goto out;
    in_txn = true;

    /* Atomically claim the token (unexpired + unused), returning its user. */
    {
        char nowbuf[24]; snprintf(nowbuf, sizeof nowbuf, "%ld", cel_now_epoch());
        const char *p[2] = { h, nowbuf };
        int f = cel_db_one_text(c,
            "UPDATE cel_password_resets SET used_at=unixepoch() "
            "WHERE token=?1 AND used_at IS NULL AND expires_at > ?2 "
            "RETURNING user_id", p, 2, user_id, sizeof user_id);
        if (f != 1 || !user_id[0]) { rc = CEL_AUTH_INVALID; goto out; }
    }

    /* Set the new secret on the password identity. */
    {
        const char *up[2] = { hash, user_id };
        if (!cel_db_exec(c, "UPDATE cel_identities SET secret=?1 "
                       "WHERE user_id=?2 AND provider='password'", up, 2) || sqlite3_changes(c) != 1) {
            rc = CEL_AUTH_INVALID; goto out;
        }
    }

    /* Revoke sessions + pending MFA challenges, clear lockout (full recovery, L-5). */
    {
        const char *us[1] = { user_id };
        cel_db_exec(c, "DELETE FROM cel_sessions WHERE user_id=?1", us, 1);
        cel_db_exec(c, "DELETE FROM cel_mfa_challenges WHERE user_id=?1", us, 1);
        lockout_reset(c, user_id);
    }

    if (!tx(c, "COMMIT")) goto out;
    in_txn = false;
    cel_session_cache_clear();   /* can't evict by user id — clear and let it refill */
    rc = CEL_AUTH_OK;
out:
    if (in_txn) tx(c, "ROLLBACK");
    app_db_conn_release(app, c);
    app_db_write_unlock(app);
    return rc;
}

#define VERIFY_TTL_SECONDS 86400   /* an email-verification link is good for 24h */

int cel_auth_create_email_verification(const char *user_id,
                                       char *out_token, size_t token_size,
                                       char *out_email, size_t email_size) {
    app_db_t *app = app_db_current();
    if (!app) return CEL_AUTH_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) { app_db_write_unlock(app); return CEL_AUTH_DBERR; }
    int rc = CEL_AUTH_DBERR;

    /* Look up the account email + whether it's already verified. */
    bool already = false;
    {
        const char *p[1] = { user_id };
        sqlite3_stmt *st;
        if (cel_db_prep(c, "SELECT email, (email_verified_at IS NOT NULL) FROM cel_users WHERE id=?1",
                   p, 1, &st) != SQLITE_OK) goto out;
        int step = sqlite3_step(st);
        if (step == SQLITE_ROW) {
            snprintf(out_email, email_size, "%s", (const char *)sqlite3_column_text(st, 0));
            already = sqlite3_column_int(st, 1) != 0;
        }
        sqlite3_finalize(st);
        if (step != SQLITE_ROW) { rc = CEL_AUTH_INVALID; goto out; }
    }
    if (already) { rc = CEL_AUTH_CONFLICT; goto out; }   /* nothing to do */

    if (cel_random_token_hex(out_token, token_size, TOKEN_BYTES) != 0) goto out;
    {
        char hbuf[65];
        if (cel_token_hash(out_token, hbuf, sizeof hbuf) != 0) goto out;
        char exp[24]; snprintf(exp, sizeof exp, "%ld", cel_now_epoch() + VERIFY_TTL_SECONDS);
        const char *ins[3] = { hbuf, user_id, exp };
        rc = cel_db_exec(c, "INSERT INTO cel_email_verifications(token, user_id, expires_at) "
                       "VALUES(?1, ?2, ?3)", ins, 3) ? CEL_AUTH_OK : CEL_AUTH_DBERR;
    }
out:
    app_db_conn_release(app, c);
    app_db_write_unlock(app);
    return rc;
}

int cel_auth_verify_email(const char *token) {
    char h[65];
    if (cel_token_hash(token, h, sizeof h) != 0) return CEL_AUTH_INVALID;

    app_db_t *app = app_db_current();
    if (!app) return CEL_AUTH_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) { app_db_write_unlock(app); return CEL_AUTH_DBERR; }
    int rc = CEL_AUTH_DBERR;
    bool in_txn = false;
    char user_id[37] = {0};

    if (!tx(c, "BEGIN")) goto out;
    in_txn = true;

    /* Atomically claim the token (unexpired + unused). */
    {
        char nowbuf[24]; snprintf(nowbuf, sizeof nowbuf, "%ld", cel_now_epoch());
        const char *p[2] = { h, nowbuf };
        int f = cel_db_one_text(c,
            "UPDATE cel_email_verifications SET used_at=unixepoch() "
            "WHERE token=?1 AND used_at IS NULL AND expires_at > ?2 "
            "RETURNING user_id", p, 2, user_id, sizeof user_id);
        if (f != 1 || !user_id[0]) { rc = CEL_AUTH_INVALID; goto out; }
    }

    /* Mark the email verified (idempotent: keep the first verification time). */
    {
        const char *up[1] = { user_id };
        if (!cel_db_exec(c, "UPDATE cel_users SET email_verified_at=COALESCE(email_verified_at, unixepoch()) "
                       "WHERE id=?1", up, 1)) goto out;
    }

    if (!tx(c, "COMMIT")) goto out;
    in_txn = false;
    cel_session_cache_clear();   /* refresh the cached email_verified flag */
    rc = CEL_AUTH_OK;
out:
    if (in_txn) tx(c, "ROLLBACK");
    app_db_conn_release(app, c);
    app_db_write_unlock(app);
    return rc;
}

int cel_auth_seed_user(const char *email, const char *password, const char *role) {
    char hash[256];
    if (cel_password_hash(password, hash, sizeof hash) != 0) return -1;

    app_db_t *app = app_db_current();
    if (!app) return -1;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) { app_db_write_unlock(app); return -1; }
    int rc = -1;
    bool in_txn = false;
    char uid[37] = {0};

    if (!tx(c, "BEGIN")) goto out;
    in_txn = true;

    /* Upsert the account (role/active refreshed on re-seed); RETURNING gives the
     * id whether the row was inserted or updated. */
    {
        char nid[37]; cel_uuid_v4(nid, sizeof nid);
        const char *p[3] = { nid, email, role };
        int f = cel_db_one_text(c,
            "INSERT INTO cel_users(id, email, role) VALUES(?1, ?2, ?3) "
            "ON CONFLICT(email) DO UPDATE SET role=excluded.role, is_active=1 "
            "RETURNING id", p, 3, uid, sizeof uid);
        if (f != 1 || !uid[0]) { LOG_ERROR("seed user failed: %s", sqlite3_errmsg(c)); goto out; }
    }

    /* Upsert the password identity (re-seed updates the stored hash). */
    {
        char iid[37]; cel_uuid_v4(iid, sizeof iid);
        const char *p[4] = { iid, uid, email, hash };
        if (!cel_db_exec(c, "INSERT INTO cel_identities(id, user_id, provider, provider_uid, secret) "
                       "VALUES(?1, ?2, 'password', ?3, ?4) "
                       "ON CONFLICT(provider, provider_uid) DO UPDATE SET secret=excluded.secret", p, 4)) {
            LOG_ERROR("seed identity failed: %s", sqlite3_errmsg(c));
            goto out;
        }
    }

    if (!tx(c, "COMMIT")) goto out;
    in_txn = false;
    rc = 0;
out:
    if (in_txn) tx(c, "ROLLBACK");
    app_db_conn_release(app, c);
    app_db_write_unlock(app);
    return rc;
}

int cel_auth_seed_admin(const char *email, const char *password) {
    return cel_auth_seed_user(email, password, "admin");
}
