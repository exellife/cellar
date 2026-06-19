#include "auth.h"
#include "password.h"
#include "db_connection.h"
#include "session_cache.h"
#include "mfa.h"
#include "metrics.h"
#include "logger.h"

#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TOKEN_BYTES 32   /* 256-bit -> 64 hex chars */

/* A decoy password hash, verified against when an email is not found, so that a
 * missing user takes the same time as a wrong password (no enumeration oracle).
 * Computed once at startup via pgf_auth_init(). */
static char g_decoy_hash[256];

void pgf_auth_init(void) {
    if (pgf_password_hash("pgforge-decoy-password-never-matches", g_decoy_hash,
                          sizeof g_decoy_hash) != 0)
        g_decoy_hash[0] = '\0';
}

/* Run a transaction-control statement (BEGIN/COMMIT/ROLLBACK); true on success.
 * Credential writes now touch two tables (pgf_users + pgf_identities), so they
 * run in a transaction to stay atomic. */
static bool tx(PGconn *c, const char *cmd) {
    PGresult *r = PQexec(c, cmd);
    bool ok = PQresultStatus(r) == PGRES_COMMAND_OK;
    PQclear(r);
    return ok;
}

/* Per-account login lockout config (0 = disabled, the default). */
static int g_lockout_n = 0;   /* failures before lock */
static int g_lockout_w = 0;   /* streak window AND lock duration (seconds) */

void pgf_auth_set_lockout(int limit, int window_seconds) {
    g_lockout_n = (limit > 0 && window_seconds > 0) ? limit : 0;
    g_lockout_w = (limit > 0 && window_seconds > 0) ? window_seconds : 0;
}

/* Record one failed login: extend the streak (or start a fresh one if the last
 * failure was outside the window), and lock the account once it hits the limit. */
static void lockout_record_failure(PGconn *c, const char *user_id, int failed, long last_failed) {
    time_t now = time(NULL);
    int newcount = (last_failed == 0 || (now - last_failed) > g_lockout_w) ? 1 : failed + 1;
    char cnt[16], lim[16], win[16];
    snprintf(cnt, sizeof cnt, "%d", newcount);
    snprintf(lim, sizeof lim, "%d", g_lockout_n);
    snprintf(win, sizeof win, "%d", g_lockout_w);
    const char *p[4] = { user_id, cnt, lim, win };
    PQclear(PQexecParams(c,
        "UPDATE pgf_users SET failed_login_count=$2::int, last_failed_login_at=now(), "
        "locked_until = CASE WHEN $2::int >= $3::int "
        "THEN now() + ($4::int * interval '1 second') ELSE NULL END "
        "WHERE id=$1::uuid",
        4, NULL, p, NULL, NULL, 0));
}

static void lockout_reset(PGconn *c, const char *user_id) {
    const char *p[1] = { user_id };
    PQclear(PQexecParams(c,
        "UPDATE pgf_users SET failed_login_count=0, last_failed_login_at=NULL, locked_until=NULL "
        "WHERE id=$1::uuid",
        1, NULL, p, NULL, NULL, 0));
}

int pgf_auth_unlock(const char *email) {
    if (!email || !*email) return -1;
    PGconn *c = db_connection_acquire();
    if (!c) return -1;
    const char *p[1] = { email };
    PGresult *r = PQexecParams(c,
        "UPDATE pgf_users SET failed_login_count=0, last_failed_login_at=NULL, locked_until=NULL "
        "WHERE email=$1",
        1, NULL, p, NULL, NULL, 0);
    int n = (PQresultStatus(r) == PGRES_COMMAND_OK) ? atoi(PQcmdTuples(r)) : -1;
    PQclear(r);
    db_connection_release(c);
    return n;
}

int pgf_auth_login(const char *email, const char *password, int ttl_seconds,
                   char *out_token, size_t token_size,
                   char *out_challenge, size_t challenge_size,
                   pgf_user_t *out_user) {
    PGconn *c = db_connection_acquire();
    if (!c) return PGF_AUTH_DBERR;
    int rc = PGF_AUTH_DBERR;

    /* Resolve the password IDENTITY (provider_uid = email) and its user. This is
     * the same shape a federated login uses (provider='google', uid=the 'sub'). */
    const char *p[1] = { email };
    PGresult *r = PQexecParams(c,
        "SELECT u.id::text, i.secret, u.role, u.is_active, u.email, "
        "to_jsonb(u) ->> 'tenant_id', (u.email_verified_at IS NOT NULL), "
        "(u.locked_until IS NOT NULL AND u.locked_until > now()), u.failed_login_count, "
        "COALESCE(EXTRACT(EPOCH FROM u.last_failed_login_at)::bigint, 0) "
        "FROM pgf_identities i JOIN pgf_users u ON u.id = i.user_id "
        "WHERE i.provider = 'password' AND i.provider_uid = $1",
        1, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); goto out; }
    if (PQntuples(r) != 1) {
        PQclear(r);
        if (g_decoy_hash[0]) pgf_password_verify(g_decoy_hash, password);  /* equalize timing */
        rc = PGF_AUTH_INVALID;
        goto out;
    }

    {
        /* Copy what we need, then free the result before the lockout UPDATEs. */
        char uid[37], hash[256], role[32], uemail[256], tenant[37];
        snprintf(uid,    sizeof uid,    "%s", PQgetvalue(r, 0, 0));
        snprintf(hash,   sizeof hash,   "%s", PQgetisnull(r, 0, 1) ? "" : PQgetvalue(r, 0, 1));
        snprintf(role,   sizeof role,   "%s", PQgetvalue(r, 0, 2));
        bool active = strcmp(PQgetvalue(r, 0, 3), "t") == 0;
        snprintf(uemail, sizeof uemail, "%s", PQgetvalue(r, 0, 4));
        snprintf(tenant, sizeof tenant, "%s", PQgetisnull(r, 0, 5) ? "" : PQgetvalue(r, 0, 5));
        bool email_verified = strcmp(PQgetvalue(r, 0, 6), "t") == 0;
        bool locked      = strcmp(PQgetvalue(r, 0, 7), "t") == 0;
        int  failed      = atoi(PQgetvalue(r, 0, 8));
        long last_failed = atol(PQgetvalue(r, 0, 9));
        PQclear(r);

        /* Always verify (even when inactive or the secret is absent) so timing
         * doesn't distinguish the cases — a null secret falls back to the decoy. */
        bool ok;
        if (hash[0]) {
            ok = pgf_password_verify(hash, password);
        } else {
            if (g_decoy_hash[0]) pgf_password_verify(g_decoy_hash, password);
            ok = false;
        }

        if (g_lockout_n > 0) {
            if (locked) { rc = PGF_AUTH_LOCKED; goto out; }   /* locked even if pw is right */
            if (!active || !ok) {
                lockout_record_failure(c, uid, failed, last_failed);
                rc = PGF_AUTH_INVALID;
                goto out;
            }
            if (failed > 0) lockout_reset(c, uid);            /* clear the streak on success */
        } else if (!active || !ok) {
            rc = PGF_AUTH_INVALID;
            goto out;
        }

        snprintf(out_user->id,    sizeof out_user->id,    "%s", uid);
        snprintf(out_user->email, sizeof out_user->email, "%s", uemail);
        snprintf(out_user->role,  sizeof out_user->role,  "%s", role);
        snprintf(out_user->tenant_id, sizeof out_user->tenant_id, "%s", tenant);
        out_user->email_verified = email_verified;
    }
    db_connection_release(c);   /* the sub-calls below manage their own connections */
    c = NULL;

    /* Second factor? A confirmed TOTP enrollment (with MFA enabled) means we issue
     * a one-time challenge instead of a session; the caller completes the login via
     * pgf_mfa_verify_login. Otherwise mint the session immediately, as before. */
    if (pgf_mfa_required_for(out_user->id)) {
        rc = (pgf_mfa_create_challenge(out_user->id, out_challenge, challenge_size) == 0)
                 ? PGF_AUTH_MFA_REQUIRED : PGF_AUTH_DBERR;
        goto out;
    }
    rc = pgf_auth_issue_session(out_user->id, ttl_seconds, out_token, token_size);
out:
    db_connection_release(c);   /* safe on NULL */
    if      (rc == PGF_AUTH_OK)                                  pgf_metric_inc(PGF_M_LOGIN_OK);
    else if (rc == PGF_AUTH_INVALID || rc == PGF_AUTH_LOCKED)    pgf_metric_inc(PGF_M_LOGIN_FAIL);
    return rc;
}

/* Mint a session for an already-authenticated user (shared by the password path,
 * MFA verify, and future federated login). Token stored hashed; raw returned. */
int pgf_auth_issue_session(const char *user_id, int ttl_seconds,
                           char *out_token, size_t token_size) {
    if (pgf_random_token_hex(out_token, token_size, TOKEN_BYTES) != 0) return PGF_AUTH_DBERR;
    char thash[65];
    if (pgf_token_hash(out_token, thash, sizeof thash) != 0) return PGF_AUTH_DBERR;

    PGconn *c = db_connection_acquire();
    if (!c) return PGF_AUTH_DBERR;
    int rc = PGF_AUTH_DBERR;

    char ttl[16];
    snprintf(ttl, sizeof ttl, "%d", ttl_seconds);
    const char *ins[3] = { thash, user_id, ttl };
    PGresult *r = PQexecParams(c,
        "INSERT INTO pgf_sessions(token, user_id, expires_at) "
        "VALUES($1, $2::uuid, now() + ($3::int * interval '1 second'))",
        3, NULL, ins, NULL, NULL, 0);
    bool ok = PQresultStatus(r) == PGRES_COMMAND_OK;
    PQclear(r);
    if (ok) {
        const char *up[1] = { user_id };
        PQclear(PQexecParams(c, "UPDATE pgf_users SET last_login_at=now() WHERE id=$1::uuid",
                             1, NULL, up, NULL, NULL, 0));
        rc = PGF_AUTH_OK;
    }
    db_connection_release(c);
    return rc;
}

int pgf_auth_oauth_login(const char *provider, const char *sub,
                         const char *email, bool email_verified, bool email_link_trusted,
                         const char *provision_role, int ttl_seconds,
                         char *out_token, size_t token_size,
                         char *out_challenge, size_t challenge_size, pgf_user_t *out_user) {
    if (!provider || !*provider || !sub || !*sub) return PGF_AUTH_INVALID;
    PGconn *c = db_connection_acquire();
    if (!c) return PGF_AUTH_DBERR;
    int rc = PGF_AUTH_DBERR;
    char user_id[37] = {0};

    /* 1. an existing federated identity resolves straight to its user. */
    {
        const char *p[2] = { provider, sub };
        PGresult *r = PQexecParams(c,
            "SELECT user_id::text FROM pgf_identities WHERE provider=$1 AND provider_uid=$2",
            2, NULL, p, NULL, NULL, 0);
        bool ok = PQresultStatus(r) == PGRES_TUPLES_OK;
        if (ok && PQntuples(r) == 1) snprintf(user_id, sizeof user_id, "%s", PQgetvalue(r, 0, 0));
        PQclear(r);
        if (!ok) goto out;
    }

    /* 2. an account may already exist for this verified email. Auto-LINK the
     *    federated identity to it ONLY when the provider is explicitly trusted for
     *    the email's domain (email_link_trusted) — never on the provider's
     *    email_verified claim alone, which a loose/hostile IdP can forge to take
     *    over the account (H-3). An untrusted email collision is REFUSED, not
     *    silently merged; the user must link via an authenticated flow instead. */
    if (!user_id[0] && email_verified && email && *email) {
        const char *p[1] = { email };
        PGresult *r = PQexecParams(c, "SELECT id::text FROM pgf_users WHERE email=$1",
                                   1, NULL, p, NULL, NULL, 0);
        bool found = (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1);
        char existing[37] = {0};
        if (found) snprintf(existing, sizeof existing, "%s", PQgetvalue(r, 0, 0));
        bool query_ok = (PQresultStatus(r) == PGRES_TUPLES_OK);
        PQclear(r);
        if (!query_ok) goto out;
        if (found) {
            if (!email_link_trusted) { rc = PGF_AUTH_INVALID; goto out; }  /* refuse silent merge */
            snprintf(user_id, sizeof user_id, "%s", existing);
            const char *ins[3] = { user_id, provider, sub };
            PQclear(PQexecParams(c,
                "INSERT INTO pgf_identities(user_id, provider, provider_uid) VALUES($1::uuid,$2,$3) "
                "ON CONFLICT (provider, provider_uid) DO NOTHING",
                3, NULL, ins, NULL, NULL, 0));
        }
    }

    /* 3. otherwise auto-provision — only if signup is allowed (a role) and we have
     *    an email for the account. No account + can't provision => INVALID. */
    if (!user_id[0]) {
        if (!provision_role || !*provision_role || !email || !*email) { rc = PGF_AUTH_INVALID; goto out; }
        if (!tx(c, "BEGIN")) goto out;
        bool committed = false;
        const char *up[2] = { email, provision_role };
        PGresult *r = PQexecParams(c,
            "INSERT INTO pgf_users(email, role) VALUES($1,$2) RETURNING id::text",
            2, NULL, up, NULL, NULL, 0);
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1) {
            snprintf(user_id, sizeof user_id, "%s", PQgetvalue(r, 0, 0));
            PQclear(r);
            const char *ins[3] = { user_id, provider, sub };
            PGresult *r2 = PQexecParams(c,
                "INSERT INTO pgf_identities(user_id, provider, provider_uid) VALUES($1::uuid,$2,$3)",
                3, NULL, ins, NULL, NULL, 0);
            bool ok2 = PQresultStatus(r2) == PGRES_COMMAND_OK;
            PQclear(r2);
            if (ok2) committed = tx(c, "COMMIT");
        } else {
            PQclear(r);
        }
        if (!committed) { tx(c, "ROLLBACK"); goto out; }   /* rc stays DBERR */
    }

    /* A provider-verified email confirms the account's email only when the provider
     * is trusted for that domain — never stamp local email_verified_at from an
     * unauthenticated/untrusted federated claim (H-3). */
    if (email_verified && email && *email && email_link_trusted) {
        const char *uv[2] = { user_id, email };
        PQclear(PQexecParams(c,
            "UPDATE pgf_users SET email_verified_at = COALESCE(email_verified_at, now()) "
            "WHERE id=$1::uuid AND email=$2",
            2, NULL, uv, NULL, NULL, 0));
    }

    db_connection_release(c);
    c = NULL;

    /* Second factor (H-2): federated login converges on the SAME session as the
     * password path, so it must honor the SAME MFA gate. A confirmed TOTP
     * enrollment means we issue a one-time challenge instead of a session — the
     * client finishes via pgf_mfa_verify_login. Without this, enrolling TOTP and
     * then signing in via OAuth bypassed the second factor entirely. */
    if (out_challenge && challenge_size && pgf_mfa_required_for(user_id)) {
        rc = (pgf_mfa_create_challenge(user_id, out_challenge, challenge_size) == 0)
                 ? PGF_AUTH_MFA_REQUIRED : PGF_AUTH_DBERR;
        goto out;
    }
    rc = pgf_auth_issue_session(user_id, ttl_seconds, out_token, token_size);
    if (rc == PGF_AUTH_OK) {
        if (pgf_auth_verify(out_token, out_user) != PGF_AUTH_OK) rc = PGF_AUTH_DBERR;
        else pgf_metric_inc(PGF_M_LOGIN_OK);
    }
out:
    db_connection_release(c);   /* safe on NULL */
    return rc;
}

int pgf_auth_register(const char *email, const char *password, const char *role,
                      int ttl_seconds, char *out_token, size_t token_size,
                      pgf_user_t *out_user) {
    char hash[256];
    if (pgf_password_hash(password, hash, sizeof hash) != 0) return PGF_AUTH_DBERR;

    PGconn *c = db_connection_acquire();
    if (!c) return PGF_AUTH_DBERR;
    int rc = PGF_AUTH_DBERR;
    bool in_txn = false;

    /* Account + password identity + session are written atomically. A duplicate
     * email must fail (not silently take over an existing account). */
    if (!tx(c, "BEGIN")) goto out;
    in_txn = true;

    /* 1. the account row (no credential here anymore). `to_jsonb ->> 'tenant_id'`
     * yields the value in pooled mode and NULL in single-tenant. */
    {
        const char *p[2] = { email, role };
        PGresult *r = PQexecParams(c,
            "INSERT INTO pgf_users(email, role) VALUES($1, $2) "
            "RETURNING id::text, to_jsonb(pgf_users) ->> 'tenant_id'",
            2, NULL, p, NULL, NULL, 0);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
            const char *ss = PQresultErrorField(r, PG_DIAG_SQLSTATE);
            if (ss && !strcmp(ss, "23505")) rc = PGF_AUTH_CONFLICT;   /* unique_violation */
            else LOG_ERROR("register failed: %s", PQerrorMessage(c));
            PQclear(r);
            goto out;
        }
        snprintf(out_user->id,    sizeof out_user->id,    "%s", PQgetvalue(r, 0, 0));
        snprintf(out_user->email, sizeof out_user->email, "%s", email);
        snprintf(out_user->role,  sizeof out_user->role,  "%s", role);
        snprintf(out_user->tenant_id, sizeof out_user->tenant_id, "%s",
                 PQgetisnull(r, 0, 1) ? "" : PQgetvalue(r, 0, 1));
        out_user->email_verified = false;   /* freshly registered — not yet verified */
        PQclear(r);
    }

    /* 2. the password identity (provider='password', uid=email). */
    {
        const char *p[3] = { out_user->id, email, hash };
        PGresult *r = PQexecParams(c,
            "INSERT INTO pgf_identities(user_id, provider, provider_uid, secret) "
            "VALUES($1::uuid, 'password', $2, $3)",
            3, NULL, p, NULL, NULL, 0);
        bool ok = PQresultStatus(r) == PGRES_COMMAND_OK;
        if (!ok) {
            const char *ss = PQresultErrorField(r, PG_DIAG_SQLSTATE);
            if (ss && !strcmp(ss, "23505")) rc = PGF_AUTH_CONFLICT;
            else LOG_ERROR("register identity failed: %s", PQerrorMessage(c));
        }
        PQclear(r);
        if (!ok) goto out;
    }

    /* 3. the auto-login session (token hash stored; raw token returned to caller). */
    if (pgf_random_token_hex(out_token, token_size, TOKEN_BYTES) != 0) goto out;
    {
        char thash[65];
        if (pgf_token_hash(out_token, thash, sizeof thash) != 0) goto out;
        char ttl[16];
        snprintf(ttl, sizeof ttl, "%d", ttl_seconds);
        const char *ins[3] = { thash, out_user->id, ttl };
        PGresult *r2 = PQexecParams(c,
            "INSERT INTO pgf_sessions(token, user_id, expires_at) "
            "VALUES($1, $2::uuid, now() + ($3::int * interval '1 second'))",
            3, NULL, ins, NULL, NULL, 0);
        bool ok = PQresultStatus(r2) == PGRES_COMMAND_OK;
        PQclear(r2);
        if (!ok) goto out;
    }

    if (!tx(c, "COMMIT")) goto out;
    in_txn = false;
    rc = PGF_AUTH_OK;
out:
    if (in_txn) tx(c, "ROLLBACK");
    db_connection_release(c);
    return rc;
}

int pgf_auth_create_user(const char *email, const char *password, const char *role,
                         const char *tenant_id, char *out_id, size_t out_id_size) {
    char hash[256];
    if (pgf_password_hash(password, hash, sizeof hash) != 0) return PGF_AUTH_DBERR;

    PGconn *c = db_connection_acquire();
    if (!c) return PGF_AUTH_DBERR;
    int rc = PGF_AUTH_DBERR;
    bool in_txn = false;
    char uid[37] = {0};

    if (!tx(c, "BEGIN")) goto out;
    in_txn = true;

    /* 1. the account (optionally bound to a tenant in pooled mode). */
    {
        bool with_tenant = tenant_id && tenant_id[0];
        PGresult *r;
        if (with_tenant) {
            const char *p[3] = { email, role, tenant_id };
            r = PQexecParams(c,
                "INSERT INTO pgf_users(email, role, tenant_id) "
                "VALUES($1, $2, $3::uuid) RETURNING id::text",
                3, NULL, p, NULL, NULL, 0);
        } else {
            const char *p[2] = { email, role };
            r = PQexecParams(c,
                "INSERT INTO pgf_users(email, role) VALUES($1, $2) RETURNING id::text",
                2, NULL, p, NULL, NULL, 0);
        }
        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
            const char *ss = PQresultErrorField(r, PG_DIAG_SQLSTATE);
            if (ss && !strcmp(ss, "23505")) rc = PGF_AUTH_CONFLICT;       /* dup email */
            else if (ss && !strcmp(ss, "23503")) rc = PGF_AUTH_INVALID;   /* bad tenant FK */
            else LOG_ERROR("create user failed: %s", PQerrorMessage(c));
            PQclear(r);
            goto out;
        }
        snprintf(uid, sizeof uid, "%s", PQgetvalue(r, 0, 0));
        PQclear(r);
    }

    /* 2. the password identity for the new account. */
    {
        const char *p[3] = { uid, email, hash };
        PGresult *r = PQexecParams(c,
            "INSERT INTO pgf_identities(user_id, provider, provider_uid, secret) "
            "VALUES($1::uuid, 'password', $2, $3)",
            3, NULL, p, NULL, NULL, 0);
        bool ok = PQresultStatus(r) == PGRES_COMMAND_OK;
        if (!ok) {
            const char *ss = PQresultErrorField(r, PG_DIAG_SQLSTATE);
            if (ss && !strcmp(ss, "23505")) rc = PGF_AUTH_CONFLICT;
            else LOG_ERROR("create identity failed: %s", PQerrorMessage(c));
        }
        PQclear(r);
        if (!ok) goto out;
    }

    if (!tx(c, "COMMIT")) goto out;
    in_txn = false;
    snprintf(out_id, out_id_size, "%s", uid);
    rc = PGF_AUTH_OK;
out:
    if (in_txn) tx(c, "ROLLBACK");
    db_connection_release(c);
    return rc;
}

int pgf_auth_verify(const char *token, pgf_user_t *out_user) {
    PGconn *c = db_connection_acquire();
    if (!c) return PGF_AUTH_DBERR;
    int rc = PGF_AUTH_DBERR;

    char thash[65];
    if (pgf_token_hash(token, thash, sizeof thash) != 0) { db_connection_release(c); return PGF_AUTH_INVALID; }
    const char *p[1] = { thash };
    /* `to_jsonb(u) ->> 'tenant_id'` yields the column's value when pgf_users has a
     * tenant_id column (pooled mode) and NULL when it doesn't (single-tenant) —
     * so the same query works in both deployments without dynamic SQL. */
    PGresult *r = PQexecParams(c,
        "SELECT u.id::text, u.email, u.role, to_jsonb(u) ->> 'tenant_id', "
        "(u.email_verified_at IS NOT NULL) "
        "FROM pgf_sessions s JOIN pgf_users u ON u.id = s.user_id "
        "WHERE s.token=$1 AND s.expires_at > now() AND u.is_active",
        1, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); goto out; }
    if (PQntuples(r) != 1) { PQclear(r); rc = PGF_AUTH_INVALID; goto out; }

    snprintf(out_user->id,    sizeof out_user->id,    "%s", PQgetvalue(r, 0, 0));
    snprintf(out_user->email, sizeof out_user->email, "%s", PQgetvalue(r, 0, 1));
    snprintf(out_user->role,  sizeof out_user->role,  "%s", PQgetvalue(r, 0, 2));
    snprintf(out_user->tenant_id, sizeof out_user->tenant_id, "%s",
             PQgetisnull(r, 0, 3) ? "" : PQgetvalue(r, 0, 3));
    out_user->email_verified = strcmp(PQgetvalue(r, 0, 4), "t") == 0;
    PQclear(r);
    /* NB: token verification is read-only and runs on every request — we no longer
     * write last_seen_at here (it made every authed request a write). */
    rc = PGF_AUTH_OK;
out:
    db_connection_release(c);
    return rc;
}

int pgf_auth_resolve(const char *token, pgf_user_t *out_user) {
    if (pgf_session_cache_get(token, out_user)) return PGF_AUTH_OK;   /* skip the DB */
    int rc = pgf_auth_verify(token, out_user);
    if (rc == PGF_AUTH_OK) pgf_session_cache_put(token, out_user);
    return rc;
}

int pgf_auth_logout(const char *token) {
    pgf_session_cache_evict(token);   /* revoke immediately on this instance */
    char thash[65];
    if (pgf_token_hash(token, thash, sizeof thash) != 0) return PGF_AUTH_INVALID;
    PGconn *c = db_connection_acquire();
    if (!c) return PGF_AUTH_DBERR;
    const char *p[1] = { thash };
    PGresult *r = PQexecParams(c,
        "DELETE FROM pgf_sessions WHERE token=$1", 1, NULL, p, NULL, NULL, 0);
    int rc = (PQresultStatus(r) == PGRES_COMMAND_OK) ? PGF_AUTH_OK : PGF_AUTH_DBERR;
    PQclear(r);
    db_connection_release(c);
    return rc;
}

int pgf_auth_revoke_user_sessions(const char *email) {
    if (!email || !*email) return -1;
    PGconn *c = db_connection_acquire();
    if (!c) return -1;
    const char *p[1] = { email };
    PGresult *r = PQexecParams(c,
        "DELETE FROM pgf_sessions WHERE user_id = (SELECT id FROM pgf_users WHERE email=$1)",
        1, NULL, p, NULL, NULL, 0);
    int n = (PQresultStatus(r) == PGRES_COMMAND_OK) ? atoi(PQcmdTuples(r)) : -1;
    PQclear(r);
    db_connection_release(c);
    /* The cache can't evict by user, so clear it (same-instance; other instances
     * re-validate within the cache TTL). */
    if (n >= 0) pgf_session_cache_clear();
    return n;
}

#define RESET_TTL_SECONDS 3600   /* a password-reset link is good for 1 hour */

int pgf_auth_create_password_reset(const char *email, char *out_token, size_t token_size) {
    PGconn *c = db_connection_acquire();
    if (!c) return PGF_AUTH_DBERR;
    int rc = PGF_AUTH_DBERR;

    /* Only an account that actually has a password identity can reset a password. */
    char user_id[37] = {0};
    {
        const char *p[1] = { email };
        PGresult *r = PQexecParams(c,
            "SELECT u.id::text FROM pgf_identities i JOIN pgf_users u ON u.id = i.user_id "
            "WHERE i.provider='password' AND i.provider_uid=$1",
            1, NULL, p, NULL, NULL, 0);
        bool ok = PQresultStatus(r) == PGRES_TUPLES_OK;
        if (ok && PQntuples(r) == 1) snprintf(user_id, sizeof user_id, "%s", PQgetvalue(r, 0, 0));
        PQclear(r);
        if (!ok) goto out;
        if (!user_id[0]) { rc = PGF_AUTH_INVALID; goto out; }   /* no account — caller still 200 */
    }

    if (pgf_random_token_hex(out_token, token_size, TOKEN_BYTES) != 0) goto out;
    {
        char h[65];
        if (pgf_token_hash(out_token, h, sizeof h) != 0) goto out;
        char ttl[16];
        snprintf(ttl, sizeof ttl, "%d", RESET_TTL_SECONDS);
        const char *ins[3] = { h, user_id, ttl };
        PGresult *w = PQexecParams(c,
            "INSERT INTO pgf_password_resets(token, user_id, expires_at) "
            "VALUES($1, $2::uuid, now() + ($3::int * interval '1 second'))",
            3, NULL, ins, NULL, NULL, 0);
        rc = (PQresultStatus(w) == PGRES_COMMAND_OK) ? PGF_AUTH_OK : PGF_AUTH_DBERR;
        PQclear(w);
    }
out:
    db_connection_release(c);
    return rc;
}

int pgf_auth_perform_password_reset(const char *token, const char *new_password) {
    char hash[256];
    if (pgf_password_hash(new_password, hash, sizeof hash) != 0) return PGF_AUTH_DBERR;
    char h[65];
    if (pgf_token_hash(token, h, sizeof h) != 0) return PGF_AUTH_INVALID;

    PGconn *c = db_connection_acquire();
    if (!c) return PGF_AUTH_DBERR;
    int rc = PGF_AUTH_DBERR;
    bool in_txn = false;
    char user_id[37] = {0};

    if (!tx(c, "BEGIN")) goto out;
    in_txn = true;

    /* Atomically claim the token (unexpired + unused) — concurrent redeems can't
     * both win because of the used_at IS NULL gate under the row lock. */
    {
        const char *p[1] = { h };
        PGresult *r = PQexecParams(c,
            "UPDATE pgf_password_resets SET used_at=now() "
            "WHERE token=$1 AND used_at IS NULL AND expires_at > now() "
            "RETURNING user_id::text",
            1, NULL, p, NULL, NULL, 0);
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1)
            snprintf(user_id, sizeof user_id, "%s", PQgetvalue(r, 0, 0));
        PQclear(r);
        if (!user_id[0]) { rc = PGF_AUTH_INVALID; goto out; }
    }

    /* Set the new secret on the password identity. */
    {
        const char *up[2] = { hash, user_id };
        PGresult *r = PQexecParams(c,
            "UPDATE pgf_identities SET secret=$1 WHERE user_id=$2::uuid AND provider='password'",
            2, NULL, up, NULL, NULL, 0);
        bool ok = (PQresultStatus(r) == PGRES_COMMAND_OK && atoi(PQcmdTuples(r)) == 1);
        PQclear(r);
        if (!ok) { rc = PGF_AUTH_INVALID; goto out; }
    }

    /* Revoke existing sessions — a reset logs the user out everywhere. */
    {
        const char *us[1] = { user_id };
        PQclear(PQexecParams(c, "DELETE FROM pgf_sessions WHERE user_id=$1::uuid",
                             1, NULL, us, NULL, NULL, 0));
        /* L-5: a reset is a full account recovery. Also drop any pending MFA
         * challenges (invalidate an in-flight two-step login) and clear an account
         * lockout — otherwise a locked-out user who resets their password is still
         * refused, defeating "reset your password to regain access". */
        PQclear(PQexecParams(c, "DELETE FROM pgf_mfa_challenges WHERE user_id=$1::uuid",
                             1, NULL, us, NULL, NULL, 0));
        lockout_reset(c, user_id);
    }

    if (!tx(c, "COMMIT")) goto out;
    in_txn = false;
    pgf_session_cache_clear();   /* can't evict by user id — clear and let it refill */
    rc = PGF_AUTH_OK;
out:
    if (in_txn) tx(c, "ROLLBACK");
    db_connection_release(c);
    return rc;
}

#define VERIFY_TTL_SECONDS 86400   /* an email-verification link is good for 24h */

int pgf_auth_create_email_verification(const char *user_id,
                                       char *out_token, size_t token_size,
                                       char *out_email, size_t email_size) {
    PGconn *c = db_connection_acquire();
    if (!c) return PGF_AUTH_DBERR;
    int rc = PGF_AUTH_DBERR;

    /* Look up the account email + whether it's already verified. */
    bool already = false;
    {
        const char *p[1] = { user_id };
        PGresult *r = PQexecParams(c,
            "SELECT email, (email_verified_at IS NOT NULL) FROM pgf_users WHERE id=$1::uuid",
            1, NULL, p, NULL, NULL, 0);
        bool ok = PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1;
        if (ok) {
            snprintf(out_email, email_size, "%s", PQgetvalue(r, 0, 0));
            already = strcmp(PQgetvalue(r, 0, 1), "t") == 0;
        }
        PQclear(r);
        if (!ok) { rc = PGF_AUTH_INVALID; goto out; }
    }
    if (already) { rc = PGF_AUTH_CONFLICT; goto out; }   /* nothing to do */

    if (pgf_random_token_hex(out_token, token_size, TOKEN_BYTES) != 0) goto out;
    {
        char h[65];
        if (pgf_token_hash(out_token, h, sizeof h) != 0) goto out;
        char ttl[16];
        snprintf(ttl, sizeof ttl, "%d", VERIFY_TTL_SECONDS);
        const char *ins[3] = { h, user_id, ttl };
        PGresult *w = PQexecParams(c,
            "INSERT INTO pgf_email_verifications(token, user_id, expires_at) "
            "VALUES($1, $2::uuid, now() + ($3::int * interval '1 second'))",
            3, NULL, ins, NULL, NULL, 0);
        rc = (PQresultStatus(w) == PGRES_COMMAND_OK) ? PGF_AUTH_OK : PGF_AUTH_DBERR;
        PQclear(w);
    }
out:
    db_connection_release(c);
    return rc;
}

int pgf_auth_verify_email(const char *token) {
    char h[65];
    if (pgf_token_hash(token, h, sizeof h) != 0) return PGF_AUTH_INVALID;

    PGconn *c = db_connection_acquire();
    if (!c) return PGF_AUTH_DBERR;
    int rc = PGF_AUTH_DBERR;
    bool in_txn = false;
    char user_id[37] = {0};

    if (!tx(c, "BEGIN")) goto out;
    in_txn = true;

    /* Atomically claim the token (unexpired + unused). */
    {
        const char *p[1] = { h };
        PGresult *r = PQexecParams(c,
            "UPDATE pgf_email_verifications SET used_at=now() "
            "WHERE token=$1 AND used_at IS NULL AND expires_at > now() "
            "RETURNING user_id::text",
            1, NULL, p, NULL, NULL, 0);
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1)
            snprintf(user_id, sizeof user_id, "%s", PQgetvalue(r, 0, 0));
        PQclear(r);
        if (!user_id[0]) { rc = PGF_AUTH_INVALID; goto out; }
    }

    /* Mark the email verified (idempotent: keep the first verification time). */
    {
        const char *up[1] = { user_id };
        PGresult *r = PQexecParams(c,
            "UPDATE pgf_users SET email_verified_at = COALESCE(email_verified_at, now()) "
            "WHERE id=$1::uuid",
            1, NULL, up, NULL, NULL, 0);
        bool ok = PQresultStatus(r) == PGRES_COMMAND_OK;
        PQclear(r);
        if (!ok) goto out;
    }

    if (!tx(c, "COMMIT")) goto out;
    in_txn = false;
    pgf_session_cache_clear();   /* refresh the cached email_verified flag */
    rc = PGF_AUTH_OK;
out:
    if (in_txn) tx(c, "ROLLBACK");
    db_connection_release(c);
    return rc;
}

int pgf_auth_seed_user(const char *email, const char *password, const char *role) {
    char hash[256];
    if (pgf_password_hash(password, hash, sizeof hash) != 0) return -1;

    PGconn *c = db_connection_acquire();
    if (!c) return -1;
    int rc = -1;
    bool in_txn = false;
    char uid[37] = {0};

    if (!tx(c, "BEGIN")) goto out;
    in_txn = true;

    /* Upsert the account (role/active refreshed on re-seed); RETURNING gives the
     * id whether the row was inserted or updated. */
    {
        const char *p[2] = { email, role };
        PGresult *r = PQexecParams(c,
            "INSERT INTO pgf_users(email, role) VALUES($1, $2) "
            "ON CONFLICT(email) DO UPDATE SET role=excluded.role, is_active=true "
            "RETURNING id::text",
            2, NULL, p, NULL, NULL, 0);
        bool ok = (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1);
        if (ok) snprintf(uid, sizeof uid, "%s", PQgetvalue(r, 0, 0));
        else LOG_ERROR("seed user failed: %s", PQerrorMessage(c));
        PQclear(r);
        if (!ok) goto out;
    }

    /* Upsert the password identity (re-seed updates the stored hash). */
    {
        const char *p[3] = { uid, email, hash };
        PGresult *r = PQexecParams(c,
            "INSERT INTO pgf_identities(user_id, provider, provider_uid, secret) "
            "VALUES($1::uuid, 'password', $2, $3) "
            "ON CONFLICT(provider, provider_uid) DO UPDATE SET secret=excluded.secret",
            3, NULL, p, NULL, NULL, 0);
        bool ok = PQresultStatus(r) == PGRES_COMMAND_OK;
        if (!ok) LOG_ERROR("seed identity failed: %s", PQerrorMessage(c));
        PQclear(r);
        if (!ok) goto out;
    }

    if (!tx(c, "COMMIT")) goto out;
    in_txn = false;
    rc = 0;
out:
    if (in_txn) tx(c, "ROLLBACK");
    db_connection_release(c);
    return rc;
}

int pgf_auth_seed_admin(const char *email, const char *password) {
    return pgf_auth_seed_user(email, password, "admin");
}
