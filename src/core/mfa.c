#include "mfa.h"
#include "totp.h"
#include "password.h"        /* cel_random_token_hex, cel_token_hash, cel_uuid_v4 */
#include "app_db.h"
#include "db_sqlite.h"
#include "metrics.h"
#include "logger.h"

#include <sqlite3.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHALLENGE_TTL_SECONDS 300   /* a login challenge is good for 5 minutes */
#define MAX_CODE_ATTEMPTS     5     /* per challenge, then it's burned */
#define MAX_MFA_FAILURES      10    /* per-USER verify failures (across challenges) before lockout (M-1) */
#define MFA_LOCKOUT_SECONDS   900   /* MFA verification locked for 15 min once tripped */
#define TOTP_WINDOW           1     /* ±1 step (±30s) clock-drift tolerance */
#define MFA_ISSUER            "cellar"

static int g_mode = CEL_MFA_MODE_OFF;

void cel_mfa_set_mode(int mode) { g_mode = mode; }
int  cel_mfa_mode(void)         { return g_mode; }

/* Transaction control on `c`; true on success. */
static bool tx(sqlite3 *c, const char *cmd) {
    return sqlite3_exec(c, cmd, NULL, NULL, NULL) == SQLITE_OK;
}

bool cel_mfa_required_for(const char *user_id) {
    if (g_mode == CEL_MFA_MODE_OFF || !user_id || !*user_id) return false;
    app_db_t *app = app_db_current();
    if (!app) return false;
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) return false;
    char buf[4];
    const char *p[1] = { user_id };
    int f = cel_db_one_text(c, "SELECT 1 FROM cel_mfa WHERE user_id=?1 AND confirmed_at IS NOT NULL",
                            p, 1, buf, sizeof buf);
    app_db_conn_release(app, c);
    return f == 1;
}

int cel_mfa_enroll(const char *user_id,
                   char *out_secret, size_t secret_size,
                   char *out_uri, size_t uri_size) {
    if (g_mode == CEL_MFA_MODE_OFF) return CEL_MFA_DISABLED;
    app_db_t *app = app_db_current();
    if (!app) return CEL_MFA_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) { app_db_write_unlock(app); return CEL_MFA_DBERR; }
    int rc = CEL_MFA_DBERR;

    /* Already fully enrolled (must disable before re-enrolling), plus the email
     * for the QR label — one round trip. */
    char account[256] = "user";
    {
        const char *p[1] = { user_id };
        sqlite3_stmt *st;
        if (cel_db_prep(c, "SELECT u.email, (m.confirmed_at IS NOT NULL) "
                           "FROM cel_users u LEFT JOIN cel_mfa m ON m.user_id = u.id "
                           "WHERE u.id=?1", p, 1, &st) != SQLITE_OK) goto out;
        int step = sqlite3_step(st);
        if (step != SQLITE_ROW) { sqlite3_finalize(st); goto out; }
        snprintf(account, sizeof account, "%s", (const char *)sqlite3_column_text(st, 0));
        bool confirmed = sqlite3_column_int(st, 1) != 0;
        sqlite3_finalize(st);
        if (confirmed) { rc = CEL_MFA_ALREADY; goto out; }
    }

    if (cel_totp_generate_secret(out_secret, secret_size) != 0) goto out;
    if (cel_totp_uri(out_secret, MFA_ISSUER, account, out_uri, uri_size) != 0) goto out;

    {
        const char *ins[2] = { user_id, out_secret };
        if (cel_db_exec(c, "INSERT INTO cel_mfa(user_id, secret) VALUES(?1, ?2) "
                           "ON CONFLICT(user_id) DO UPDATE SET secret=excluded.secret, "
                           "confirmed_at=NULL, created_at=unixepoch()", ins, 2))
            rc = CEL_MFA_OK;
    }
out:
    app_db_conn_release(app, c);
    app_db_write_unlock(app);
    return rc;
}

/* Load the user's TOTP secret into `secret` (>= 64 bytes). `require_confirmed`
 * restricts to a confirmed enrollment. Returns CEL_MFA_OK / NOT_ENROLLED / DBERR. */
static int load_secret(sqlite3 *c, const char *user_id, bool require_confirmed,
                       char *secret, size_t secret_size) {
    const char *p[1] = { user_id };
    int f = cel_db_one_text(c, require_confirmed
            ? "SELECT secret FROM cel_mfa WHERE user_id=?1 AND confirmed_at IS NOT NULL"
            : "SELECT secret FROM cel_mfa WHERE user_id=?1",
            p, 1, secret, secret_size);
    if (f < 0) return CEL_MFA_DBERR;
    return f == 1 ? CEL_MFA_OK : CEL_MFA_NOT_ENROLLED;
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
 * write the human-facing codes (with a dash) into `out`. Caller holds `c` and
 * the write lock. */
static int gen_recovery_codes(sqlite3 *c, const char *user_id,
                              char out[][CEL_MFA_RECOVERY_LEN]) {
    const char *del[1] = { user_id };
    cel_db_exec(c, "DELETE FROM cel_mfa_recovery WHERE user_id=?1", del, 1);
    for (int i = 0; i < CEL_MFA_RECOVERY_N; i++) {
        char raw[33];                                   /* M-10: 16 bytes -> 32 hex (128 bits) */
        if (cel_random_token_hex(raw, sizeof raw, 16) != 0) return CEL_MFA_DBERR;
        snprintf(out[i], CEL_MFA_RECOVERY_LEN, "%.8s-%.8s-%.8s-%.8s",
                 raw, raw + 8, raw + 16, raw + 24);     /* display form (dashes stripped on redeem) */
        char hash[65];
        if (cel_token_hash(raw, hash, sizeof hash) != 0) return CEL_MFA_DBERR;   /* hash the bare code */
        char iid[37]; cel_uuid_v4(iid, sizeof iid);
        const char *ins[3] = { iid, user_id, hash };
        if (!cel_db_exec(c, "INSERT INTO cel_mfa_recovery(id, user_id, code_hash) "
                            "VALUES(?1, ?2, ?3)", ins, 3))
            return CEL_MFA_DBERR;
    }
    return CEL_MFA_OK;
}

/* Try to consume one unused recovery code for `user_id` (single use). Caller
 * holds `c` and the write lock. Returns true if a code matched and was burned. */
static bool consume_recovery_code(sqlite3 *c, const char *user_id, const char *code) {
    char norm[64];
    normalize_code(code, norm, sizeof norm);
    if (!norm[0]) return false;
    char hash[65];
    if (cel_token_hash(norm, hash, sizeof hash) != 0) return false;
    const char *p[2] = { user_id, hash };
    if (!cel_db_exec(c, "UPDATE cel_mfa_recovery SET used_at=unixepoch() "
                        "WHERE user_id=?1 AND code_hash=?2 AND used_at IS NULL", p, 2))
        return false;
    return sqlite3_changes(c) == 1;
}

int cel_mfa_confirm(const char *user_id, const char *code,
                    char out_codes[][CEL_MFA_RECOVERY_LEN]) {
    if (g_mode == CEL_MFA_MODE_OFF) return CEL_MFA_DISABLED;
    if (!code) return CEL_MFA_INVALID;
    app_db_t *app = app_db_current();
    if (!app) return CEL_MFA_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) { app_db_write_unlock(app); return CEL_MFA_DBERR; }
    int rc;
    char secret[64];
    if ((rc = load_secret(c, user_id, false, secret, sizeof secret)) != CEL_MFA_OK) goto out;
    if (!cel_totp_verify(secret, code, TOTP_WINDOW)) { rc = CEL_MFA_INVALID; goto out; }

    if (!tx(c, "BEGIN")) { rc = CEL_MFA_DBERR; goto out; }
    {
        const char *p[1] = { user_id };
        bool ok = cel_db_exec(c, "UPDATE cel_mfa SET confirmed_at=unixepoch() WHERE user_id=?1", p, 1);
        rc = ok ? gen_recovery_codes(c, user_id, out_codes) : CEL_MFA_DBERR;
    }
    if (rc == CEL_MFA_OK && tx(c, "COMMIT")) { /* committed */ }
    else { tx(c, "ROLLBACK"); if (rc == CEL_MFA_OK) rc = CEL_MFA_DBERR; }
out:
    app_db_conn_release(app, c);
    app_db_write_unlock(app);
    return rc;
}

int cel_mfa_disable(const char *user_id, const char *code) {
    if (!code) return CEL_MFA_INVALID;
    app_db_t *app = app_db_current();
    if (!app) return CEL_MFA_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) { app_db_write_unlock(app); return CEL_MFA_DBERR; }
    int rc;
    char secret[64];
    if ((rc = load_secret(c, user_id, true, secret, sizeof secret)) != CEL_MFA_OK) goto out;
    if (!cel_totp_verify(secret, code, TOTP_WINDOW)) { rc = CEL_MFA_INVALID; goto out; }

    {
        const char *p[1] = { user_id };
        tx(c, "BEGIN");
        cel_db_exec(c, "DELETE FROM cel_mfa_challenges WHERE user_id=?1", p, 1);
        cel_db_exec(c, "DELETE FROM cel_mfa_recovery WHERE user_id=?1", p, 1);
        bool ok = cel_db_exec(c, "DELETE FROM cel_mfa WHERE user_id=?1", p, 1);
        rc = (ok && tx(c, "COMMIT")) ? CEL_MFA_OK : CEL_MFA_DBERR;
        if (rc != CEL_MFA_OK) tx(c, "ROLLBACK");
    }
out:
    app_db_conn_release(app, c);
    app_db_write_unlock(app);
    return rc;
}

int cel_mfa_regenerate_recovery(const char *user_id, const char *code,
                                char out_codes[][CEL_MFA_RECOVERY_LEN]) {
    if (!code) return CEL_MFA_INVALID;
    app_db_t *app = app_db_current();
    if (!app) return CEL_MFA_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) { app_db_write_unlock(app); return CEL_MFA_DBERR; }
    int rc;
    char secret[64];
    if ((rc = load_secret(c, user_id, true, secret, sizeof secret)) != CEL_MFA_OK) goto out;
    if (!cel_totp_verify(secret, code, TOTP_WINDOW)) { rc = CEL_MFA_INVALID; goto out; }
    if (!tx(c, "BEGIN")) { rc = CEL_MFA_DBERR; goto out; }
    rc = gen_recovery_codes(c, user_id, out_codes);
    if (rc == CEL_MFA_OK && tx(c, "COMMIT")) { /* committed */ }
    else { tx(c, "ROLLBACK"); if (rc == CEL_MFA_OK) rc = CEL_MFA_DBERR; }
out:
    app_db_conn_release(app, c);
    app_db_write_unlock(app);
    return rc;
}

int cel_mfa_create_challenge(const char *user_id, char *out_challenge, size_t size) {
    if (cel_random_token_hex(out_challenge, size, 32) != 0) return CEL_MFA_DBERR;
    char h[65];
    if (cel_token_hash(out_challenge, h, sizeof h) != 0) return CEL_MFA_DBERR;
    app_db_t *app = app_db_current();
    if (!app) return CEL_MFA_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    int rc = CEL_MFA_DBERR;
    if (c) {
        char exp[24]; snprintf(exp, sizeof exp, "%ld", cel_now_epoch() + CHALLENGE_TTL_SECONDS);
        const char *ins[3] = { h, user_id, exp };
        rc = cel_db_exec(c, "INSERT INTO cel_mfa_challenges(token, user_id, expires_at) "
                            "VALUES(?1, ?2, ?3)", ins, 3) ? CEL_MFA_OK : CEL_MFA_DBERR;
        app_db_conn_release(app, c);
    }
    app_db_write_unlock(app);
    return rc;
}

int cel_mfa_verify_login(const char *challenge, const char *code, int ttl_seconds,
                         char *out_token, size_t token_size, cel_user_t *out_user) {
    if (!challenge || !code) return CEL_MFA_INVALID;
    char h[65];
    if (cel_token_hash(challenge, h, sizeof h) != 0) return CEL_MFA_INVALID;
    app_db_t *app = app_db_current();
    if (!app) return CEL_MFA_DBERR;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) { app_db_write_unlock(app); return CEL_MFA_DBERR; }
    int rc = CEL_MFA_DBERR;
    char user_id[37] = {0};
    char secret[64];
    bool success = false;

    /* The challenge must exist, be unexpired, and under the attempt cap. */
    {
        char nowbuf[24]; snprintf(nowbuf, sizeof nowbuf, "%ld", cel_now_epoch());
        const char *p[2] = { h, nowbuf };
        sqlite3_stmt *st;
        if (cel_db_prep(c, "SELECT user_id, attempts FROM cel_mfa_challenges "
                           "WHERE token=?1 AND expires_at > ?2", p, 2, &st) != SQLITE_OK) goto out;
        int step = sqlite3_step(st);
        if (step != SQLITE_ROW) { sqlite3_finalize(st); rc = CEL_MFA_INVALID; goto out; }
        snprintf(user_id, sizeof user_id, "%s", (const char *)sqlite3_column_text(st, 0));
        int attempts = sqlite3_column_int(st, 1);
        sqlite3_finalize(st);
        if (attempts >= MAX_CODE_ATTEMPTS) {
            const char *d[1] = { h };
            cel_db_exec(c, "DELETE FROM cel_mfa_challenges WHERE token=?1", d, 1);
            rc = CEL_MFA_INVALID; goto out;
        }
    }

    /* Per-user MFA lockout (M-1): the per-challenge cap is bypassable by minting a
     * fresh challenge on each factor-1 success, so accumulate failures on the user's
     * cel_mfa row and refuse verification while locked. */
    {
        char nowbuf[24]; snprintf(nowbuf, sizeof nowbuf, "%ld", cel_now_epoch());
        const char *p[2] = { user_id, nowbuf };
        char buf[4];
        if (cel_db_one_text(c, "SELECT 1 FROM cel_mfa WHERE user_id=?1 "
                               "AND locked_until IS NOT NULL AND locked_until > ?2",
                            p, 2, buf, sizeof buf) == 1) { rc = CEL_MFA_INVALID; goto out; }
    }

    if (load_secret(c, user_id, true, secret, sizeof secret) != CEL_MFA_OK) { rc = CEL_MFA_INVALID; goto out; }

    /* Accept either a valid TOTP code or an unused recovery code (consumed on use). */
    if (!cel_totp_verify(secret, code, TOTP_WINDOW) && !consume_recovery_code(c, user_id, code)) {
        const char *p[1] = { h };
        cel_db_exec(c, "UPDATE cel_mfa_challenges SET attempts=attempts+1 WHERE token=?1", p, 1);
        /* count the failure against the user; lock once the threshold is hit (and
         * reset the counter so the lock window is the rate limiter, not the count).
         * The thresholds are trusted compile constants, inlined: an expression like
         * `failed_attempts+1` has no column affinity, so a bound text param wouldn't
         * be coerced to a number for the comparison (it would never match). */
        char sql[512];
        snprintf(sql, sizeof sql,
            "UPDATE cel_mfa SET "
            "locked_until    = CASE WHEN failed_attempts+1 >= %d THEN %ld ELSE locked_until END, "
            "failed_attempts = CASE WHEN failed_attempts+1 >= %d THEN 0  ELSE failed_attempts+1 END "
            "WHERE user_id=?1",
            MAX_MFA_FAILURES, cel_now_epoch() + MFA_LOCKOUT_SECONDS, MAX_MFA_FAILURES);
        const char *pu[1] = { user_id };
        cel_db_exec(c, sql, pu, 1);
        rc = CEL_MFA_INVALID; goto out;
    }

    /* Success: clear the user's MFA failure state and burn the challenge. */
    {
        const char *pr[1] = { user_id };
        cel_db_exec(c, "UPDATE cel_mfa SET failed_attempts=0, locked_until=NULL WHERE user_id=?1", pr, 1);
        const char *p[1] = { h };
        cel_db_exec(c, "DELETE FROM cel_mfa_challenges WHERE token=?1", p, 1);
        success = true;
    }
out:
    app_db_conn_release(app, c);
    app_db_write_unlock(app);
    if (!success) return rc;

    /* Session issuance takes its own write lock — do it after releasing ours. */
    if (cel_auth_issue_session(user_id, ttl_seconds, out_token, token_size) != CEL_AUTH_OK)
        return CEL_MFA_DBERR;
    if (cel_auth_verify(out_token, out_user) != CEL_AUTH_OK) return CEL_MFA_DBERR;
    cel_metric_inc(CEL_M_LOGIN_OK);
    return CEL_MFA_OK;
}

int cel_mfa_reset(const char *email) {
    if (!email || !*email) return -1;
    app_db_t *app = app_db_current();
    if (!app) return -1;
    app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    int n = -1;
    if (c) {
        const char *p[1] = { email };
        if (cel_db_exec(c, "DELETE FROM cel_mfa WHERE user_id = "
                           "(SELECT id FROM cel_users WHERE email=?1)", p, 1))
            n = sqlite3_changes(c);
        app_db_conn_release(app, c);
    }
    app_db_write_unlock(app);
    return n;
}
