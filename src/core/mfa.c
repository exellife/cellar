#include "mfa.h"
#include "totp.h"
#include "password.h"        /* pgf_random_token_hex, pgf_token_hash */
#include "db_connection.h"
#include "metrics.h"
#include "logger.h"

#include <libpq-fe.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHALLENGE_TTL_SECONDS 300   /* a login challenge is good for 5 minutes */
#define MAX_CODE_ATTEMPTS     5     /* per challenge, then it's burned */
#define TOTP_WINDOW           1     /* ±1 step (±30s) clock-drift tolerance */
#define MFA_ISSUER            "pgforge"

static int g_mode = PGF_MFA_MODE_OFF;

void pgf_mfa_set_mode(int mode) { g_mode = mode; }
int  pgf_mfa_mode(void)         { return g_mode; }

bool pgf_mfa_required_for(const char *user_id) {
    if (g_mode == PGF_MFA_MODE_OFF || !user_id || !*user_id) return false;
    PGconn *c = db_connection_acquire();
    if (!c) return false;
    const char *p[1] = { user_id };
    PGresult *r = PQexecParams(c,
        "SELECT 1 FROM pgf_mfa WHERE user_id=$1::uuid AND confirmed_at IS NOT NULL",
        1, NULL, p, NULL, NULL, 0);
    bool required = (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1);
    PQclear(r);
    db_connection_release(c);
    return required;
}

int pgf_mfa_enroll(const char *user_id,
                   char *out_secret, size_t secret_size,
                   char *out_uri, size_t uri_size) {
    if (g_mode == PGF_MFA_MODE_OFF) return PGF_MFA_DISABLED;

    PGconn *c = db_connection_acquire();
    if (!c) return PGF_MFA_DBERR;
    int rc = PGF_MFA_DBERR;

    /* Already fully enrolled (must disable before re-enrolling), plus the email
     * for the QR label — one round trip. */
    char account[256] = "user";
    const char *p[1] = { user_id };
    PGresult *r = PQexecParams(c,
        "SELECT u.email, (m.confirmed_at IS NOT NULL) "
        "FROM pgf_users u LEFT JOIN pgf_mfa m ON m.user_id = u.id "
        "WHERE u.id=$1::uuid",
        1, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1) { PQclear(r); goto out; }
    snprintf(account, sizeof account, "%s", PQgetvalue(r, 0, 0));
    if (strcmp(PQgetvalue(r, 0, 1), "t") == 0) { PQclear(r); rc = PGF_MFA_ALREADY; goto out; }
    PQclear(r);

    if (pgf_totp_generate_secret(out_secret, secret_size) != 0) goto out;
    if (pgf_totp_uri(out_secret, MFA_ISSUER, account, out_uri, uri_size) != 0) goto out;

    {
        const char *ins[2] = { user_id, out_secret };
        PGresult *w = PQexecParams(c,
            "INSERT INTO pgf_mfa(user_id, secret) VALUES($1::uuid, $2) "
            "ON CONFLICT(user_id) DO UPDATE SET secret=excluded.secret, "
            "confirmed_at=NULL, created_at=now()",
            2, NULL, ins, NULL, NULL, 0);
        bool ok = PQresultStatus(w) == PGRES_COMMAND_OK;
        PQclear(w);
        if (ok) rc = PGF_MFA_OK;
    }
out:
    db_connection_release(c);
    return rc;
}

/* Load the user's TOTP secret into `secret` (>= 64 bytes). `require_confirmed`
 * restricts to a confirmed enrollment. Returns PGF_MFA_OK / NOT_ENROLLED / DBERR. */
static int load_secret(PGconn *c, const char *user_id, bool require_confirmed,
                       char *secret, size_t secret_size) {
    const char *p[1] = { user_id };
    PGresult *r = PQexecParams(c,
        require_confirmed
            ? "SELECT secret FROM pgf_mfa WHERE user_id=$1::uuid AND confirmed_at IS NOT NULL"
            : "SELECT secret FROM pgf_mfa WHERE user_id=$1::uuid",
        1, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); return PGF_MFA_DBERR; }
    if (PQntuples(r) != 1) { PQclear(r); return PGF_MFA_NOT_ENROLLED; }
    snprintf(secret, secret_size, "%s", PQgetvalue(r, 0, 0));
    PQclear(r);
    return PGF_MFA_OK;
}

/* Normalize a recovery code for comparison: lowercase, drop non-alphanumerics
 * (so the displayed "abcde-12345" matches whether or not the user keeps the dash). */
static void normalize_code(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    for (const char *p = in; *p && j < outsz - 1; p++) {
        unsigned char ch = (unsigned char)*p;
        if (isalnum(ch)) out[j++] = (char)tolower(ch);
    }
    out[j] = '\0';
}

/* Replace the user's recovery codes with a fresh set: store sha256(code) and
 * write the human-facing codes (with a dash) into `out`. Caller holds `c`. */
static int gen_recovery_codes(PGconn *c, const char *user_id,
                              char out[][PGF_MFA_RECOVERY_LEN]) {
    const char *del[1] = { user_id };
    PQclear(PQexecParams(c, "DELETE FROM pgf_mfa_recovery WHERE user_id=$1::uuid",
                         1, NULL, del, NULL, NULL, 0));
    for (int i = 0; i < PGF_MFA_RECOVERY_N; i++) {
        char raw[33];                                   /* M-10: 16 bytes -> 32 hex (128 bits) */
        if (pgf_random_token_hex(raw, sizeof raw, 16) != 0) return PGF_MFA_DBERR;
        snprintf(out[i], PGF_MFA_RECOVERY_LEN, "%.8s-%.8s-%.8s-%.8s",
                 raw, raw + 8, raw + 16, raw + 24);     /* display form (dashes stripped on redeem) */
        char hash[65];
        if (pgf_token_hash(raw, hash, sizeof hash) != 0) return PGF_MFA_DBERR;   /* hash the bare code */
        const char *ins[2] = { user_id, hash };
        PGresult *r = PQexecParams(c,
            "INSERT INTO pgf_mfa_recovery(user_id, code_hash) VALUES($1::uuid, $2)",
            2, NULL, ins, NULL, NULL, 0);
        bool ok = PQresultStatus(r) == PGRES_COMMAND_OK;
        PQclear(r);
        if (!ok) return PGF_MFA_DBERR;
    }
    return PGF_MFA_OK;
}

/* Try to consume one unused recovery code for `user_id` (single use). Caller
 * holds `c`. Returns true if a code matched and was burned. */
static bool consume_recovery_code(PGconn *c, const char *user_id, const char *code) {
    char norm[64];
    normalize_code(code, norm, sizeof norm);
    if (!norm[0]) return false;
    char hash[65];
    if (pgf_token_hash(norm, hash, sizeof hash) != 0) return false;
    const char *p[2] = { user_id, hash };
    PGresult *r = PQexecParams(c,
        "UPDATE pgf_mfa_recovery SET used_at=now() "
        "WHERE user_id=$1::uuid AND code_hash=$2 AND used_at IS NULL RETURNING id",
        2, NULL, p, NULL, NULL, 0);
    bool ok = (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1);
    PQclear(r);
    return ok;
}

int pgf_mfa_confirm(const char *user_id, const char *code,
                    char out_codes[][PGF_MFA_RECOVERY_LEN]) {
    if (g_mode == PGF_MFA_MODE_OFF) return PGF_MFA_DISABLED;
    if (!code) return PGF_MFA_INVALID;

    PGconn *c = db_connection_acquire();
    if (!c) return PGF_MFA_DBERR;
    int rc;
    char secret[64];
    if ((rc = load_secret(c, user_id, false, secret, sizeof secret)) != PGF_MFA_OK) goto out;

    if (!pgf_totp_verify(secret, code, TOTP_WINDOW)) { rc = PGF_MFA_INVALID; goto out; }

    {
        const char *p[1] = { user_id };
        PGresult *r = PQexecParams(c,
            "UPDATE pgf_mfa SET confirmed_at=now() WHERE user_id=$1::uuid",
            1, NULL, p, NULL, NULL, 0);
        bool ok = PQresultStatus(r) == PGRES_COMMAND_OK;
        PQclear(r);
        rc = ok ? gen_recovery_codes(c, user_id, out_codes) : PGF_MFA_DBERR;
    }
out:
    db_connection_release(c);
    return rc;
}

int pgf_mfa_disable(const char *user_id, const char *code) {
    if (!code) return PGF_MFA_INVALID;
    PGconn *c = db_connection_acquire();
    if (!c) return PGF_MFA_DBERR;
    int rc;
    char secret[64];
    if ((rc = load_secret(c, user_id, true, secret, sizeof secret)) != PGF_MFA_OK) goto out;

    if (!pgf_totp_verify(secret, code, TOTP_WINDOW)) { rc = PGF_MFA_INVALID; goto out; }

    {
        const char *p[1] = { user_id };
        PQclear(PQexecParams(c, "DELETE FROM pgf_mfa_challenges WHERE user_id=$1::uuid",
                             1, NULL, p, NULL, NULL, 0));
        PQclear(PQexecParams(c, "DELETE FROM pgf_mfa_recovery WHERE user_id=$1::uuid",
                             1, NULL, p, NULL, NULL, 0));
        PGresult *r = PQexecParams(c, "DELETE FROM pgf_mfa WHERE user_id=$1::uuid",
                                   1, NULL, p, NULL, NULL, 0);
        rc = (PQresultStatus(r) == PGRES_COMMAND_OK) ? PGF_MFA_OK : PGF_MFA_DBERR;
        PQclear(r);
    }
out:
    db_connection_release(c);
    return rc;
}

int pgf_mfa_regenerate_recovery(const char *user_id, const char *code,
                                char out_codes[][PGF_MFA_RECOVERY_LEN]) {
    if (!code) return PGF_MFA_INVALID;
    PGconn *c = db_connection_acquire();
    if (!c) return PGF_MFA_DBERR;
    int rc;
    char secret[64];
    if ((rc = load_secret(c, user_id, true, secret, sizeof secret)) != PGF_MFA_OK) goto out;
    if (!pgf_totp_verify(secret, code, TOTP_WINDOW)) { rc = PGF_MFA_INVALID; goto out; }
    rc = gen_recovery_codes(c, user_id, out_codes);
out:
    db_connection_release(c);
    return rc;
}

int pgf_mfa_create_challenge(const char *user_id, char *out_challenge, size_t size) {
    if (pgf_random_token_hex(out_challenge, size, 32) != 0) return PGF_MFA_DBERR;
    char h[65];
    if (pgf_token_hash(out_challenge, h, sizeof h) != 0) return PGF_MFA_DBERR;

    PGconn *c = db_connection_acquire();
    if (!c) return PGF_MFA_DBERR;
    char ttl[16];
    snprintf(ttl, sizeof ttl, "%d", CHALLENGE_TTL_SECONDS);
    const char *ins[3] = { h, user_id, ttl };
    PGresult *r = PQexecParams(c,
        "INSERT INTO pgf_mfa_challenges(token, user_id, expires_at) "
        "VALUES($1, $2::uuid, now() + ($3::int * interval '1 second'))",
        3, NULL, ins, NULL, NULL, 0);
    int rc = (PQresultStatus(r) == PGRES_COMMAND_OK) ? PGF_MFA_OK : PGF_MFA_DBERR;
    PQclear(r);
    db_connection_release(c);
    return rc;
}

int pgf_mfa_verify_login(const char *challenge, const char *code, int ttl_seconds,
                         char *out_token, size_t token_size, pgf_user_t *out_user) {
    if (!challenge || !code) return PGF_MFA_INVALID;
    char h[65];
    if (pgf_token_hash(challenge, h, sizeof h) != 0) return PGF_MFA_INVALID;

    PGconn *c = db_connection_acquire();
    if (!c) return PGF_MFA_DBERR;
    int rc = PGF_MFA_DBERR;
    char user_id[37] = {0};
    char secret[64];

    /* The challenge must exist, be unexpired, and under the attempt cap. */
    const char *p[1] = { h };
    PGresult *r = PQexecParams(c,
        "SELECT user_id::text, attempts FROM pgf_mfa_challenges "
        "WHERE token=$1 AND expires_at > now()",
        1, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); goto out; }
    if (PQntuples(r) != 1) { PQclear(r); rc = PGF_MFA_INVALID; goto out; }
    snprintf(user_id, sizeof user_id, "%s", PQgetvalue(r, 0, 0));
    int attempts = atoi(PQgetvalue(r, 0, 1));
    PQclear(r);
    if (attempts >= MAX_CODE_ATTEMPTS) {
        PQclear(PQexecParams(c, "DELETE FROM pgf_mfa_challenges WHERE token=$1",
                             1, NULL, p, NULL, NULL, 0));
        rc = PGF_MFA_INVALID; goto out;
    }

    if (load_secret(c, user_id, true, secret, sizeof secret) != PGF_MFA_OK) { rc = PGF_MFA_INVALID; goto out; }

    /* Accept either a valid TOTP code or an unused recovery code (consumed on use). */
    if (!pgf_totp_verify(secret, code, TOTP_WINDOW) && !consume_recovery_code(c, user_id, code)) {
        PQclear(PQexecParams(c, "UPDATE pgf_mfa_challenges SET attempts=attempts+1 WHERE token=$1",
                             1, NULL, p, NULL, NULL, 0));
        rc = PGF_MFA_INVALID; goto out;
    }

    /* Success: burn the challenge (single use) before issuing the session. */
    PQclear(PQexecParams(c, "DELETE FROM pgf_mfa_challenges WHERE token=$1",
                         1, NULL, p, NULL, NULL, 0));
    db_connection_release(c);
    c = NULL;

    if (pgf_auth_issue_session(user_id, ttl_seconds, out_token, token_size) != PGF_AUTH_OK)
        return PGF_MFA_DBERR;
    if (pgf_auth_verify(out_token, out_user) != PGF_AUTH_OK) return PGF_MFA_DBERR;
    pgf_metric_inc(PGF_M_LOGIN_OK);
    return PGF_MFA_OK;
out:
    db_connection_release(c);
    return rc;
}

int pgf_mfa_reset(const char *email) {
    if (!email || !*email) return -1;
    PGconn *c = db_connection_acquire();
    if (!c) return -1;
    const char *p[1] = { email };
    PGresult *r = PQexecParams(c,
        "DELETE FROM pgf_mfa WHERE user_id = (SELECT id FROM pgf_users WHERE email=$1)",
        1, NULL, p, NULL, NULL, 0);
    int n = (PQresultStatus(r) == PGRES_COMMAND_OK) ? atoi(PQcmdTuples(r)) : -1;
    PQclear(r);
    db_connection_release(c);
    return n;
}
