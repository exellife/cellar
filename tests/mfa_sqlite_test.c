/* ============================================================================
 * mfa_sqlite_test — TOTP two-factor on per-app SQLite (end-to-end).
 *
 * enroll -> confirm (real TOTP code) -> required_for -> create_challenge ->
 * verify_login (with a TOTP code, then a recovery code) -> disable -> reset,
 * against a throwaway SQLite app. Exercises the real auth session issuance
 * (mfa.c calls into the converted auth.c). libpq-free.
 * ============================================================================ */
#include "mfa.h"
#include "auth.h"
#include "auth_schema.h"
#include "app_db.h"
#include "password.h"
#include "totp.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok: %s\n", msg); } \
} while (0)

#define TTL 3600

/* a valid 6-digit TOTP for `secret` at the current time */
static void code_now(const char *secret, char *out, size_t n) {
    cel_totp_code_at(secret, (uint64_t)time(NULL), out, n);
}

int main(void) {
    char path[256];
    snprintf(path, sizeof path, "/tmp/cellar_mfa_%d.db", (int)getpid());
    unlink(path);

    CHECK(cel_crypto_init() == 0, "crypto init");
    cel_auth_init();

    app_db_global_init();
    app_db_t *app = app_db_get(path);
    sqlite3 *sc = app_db_conn_acquire(app);
    CHECK(cel_auth_schema_apply(sc) == 0, "apply schema");
    app_db_conn_release(app, sc);
    app_db_set_current(app);

    cel_mfa_set_mode(CEL_MFA_MODE_OPTIONAL);   /* enable MFA */

    /* a user to enroll */
    char uid[37];
    CHECK(cel_auth_create_user("m@x.com", "password1", "admin", "", uid, sizeof uid) == CEL_AUTH_OK,
          "create user");

    /* enroll: get a secret, not yet required (unconfirmed) */
    char secret[64], uri[256];
    CHECK(cel_mfa_enroll(uid, secret, sizeof secret, uri, sizeof uri) == CEL_MFA_OK, "enroll");
    CHECK(cel_mfa_required_for(uid) == false, "not required before confirm");

    /* confirm with a real TOTP code -> recovery codes; now required */
    char codes[CEL_MFA_RECOVERY_N][CEL_MFA_RECOVERY_LEN];
    char code[16]; code_now(secret, code, sizeof code);
    CHECK(cel_mfa_confirm(uid, code, codes) == CEL_MFA_OK, "confirm with TOTP code");
    CHECK(cel_mfa_required_for(uid) == true, "required after confirm");
    CHECK(cel_mfa_enroll(uid, secret, sizeof secret, uri, sizeof uri) == CEL_MFA_ALREADY,
          "re-enroll blocked while confirmed");

    /* a bad code is rejected at confirm-time semantics via verify_login below */

    /* login second factor: challenge -> verify_login with a fresh TOTP code */
    {
        char chal[129], tok[129];
        cel_user_t u;
        CHECK(cel_mfa_create_challenge(uid, chal, sizeof chal) == CEL_MFA_OK, "create challenge");
        char c2[16]; code_now(secret, c2, sizeof c2);
        int rc = cel_mfa_verify_login(chal, c2, TTL, tok, sizeof tok, &u);
        CHECK(rc == CEL_MFA_OK, "verify_login with TOTP");
        CHECK(strcmp(u.id, uid) == 0, "verify_login resolved the user");
        CHECK(cel_auth_verify(tok, &u) == CEL_AUTH_OK, "issued session token is valid");
    }

    /* a wrong code on a fresh challenge is rejected */
    {
        char chal[129], tok[129];
        cel_user_t u;
        cel_mfa_create_challenge(uid, chal, sizeof chal);
        CHECK(cel_mfa_verify_login(chal, "000000", TTL, tok, sizeof tok, &u) == CEL_MFA_INVALID,
              "verify_login wrong code -> invalid");
    }

    /* a recovery code works once, then is burned */
    {
        char chal[129], tok[129];
        cel_user_t u;
        cel_mfa_create_challenge(uid, chal, sizeof chal);
        CHECK(cel_mfa_verify_login(chal, codes[0], TTL, tok, sizeof tok, &u) == CEL_MFA_OK,
              "verify_login with recovery code");
        char chal2[129];
        cel_mfa_create_challenge(uid, chal2, sizeof chal2);
        CHECK(cel_mfa_verify_login(chal2, codes[0], TTL, tok, sizeof tok, &u) == CEL_MFA_INVALID,
              "recovery code is single-use");
    }

    /* M-1: per-user lockout — failing verify across many fresh challenges locks
     * MFA verification for the user, so even a valid code is then rejected. */
    {
        char chal[129], tok[129]; cel_user_t u;
        for (int i = 0; i < 10; i++) {                 /* MAX_MFA_FAILURES */
            cel_mfa_create_challenge(uid, chal, sizeof chal);
            cel_mfa_verify_login(chal, "000000", TTL, tok, sizeof tok, &u);  /* wrong, fresh challenge each time */
        }
        cel_mfa_create_challenge(uid, chal, sizeof chal);
        char good[16]; code_now(secret, good, sizeof good);
        CHECK(cel_mfa_verify_login(chal, good, TTL, tok, sizeof tok, &u) == CEL_MFA_INVALID,
              "MFA locked after repeated failures — a VALID code is rejected (M-1)");
    }

    /* disable requires a valid code; then MFA is no longer required (disable is not
     * gated by the verify-lockout, so admin/user recovery still works). */
    {
        char c3[16]; code_now(secret, c3, sizeof c3);
        CHECK(cel_mfa_disable(uid, c3) == CEL_MFA_OK, "disable with TOTP code (works while verify-locked)");
        CHECK(cel_mfa_required_for(uid) == false, "not required after disable");
    }

    /* admin reset: re-enroll + confirm, then reset by email drops it */
    {
        char s2[64], u2[256], code2[16], cc[CEL_MFA_RECOVERY_N][CEL_MFA_RECOVERY_LEN];
        cel_mfa_enroll(uid, s2, sizeof s2, u2, sizeof u2);
        code_now(s2, code2, sizeof code2);
        cel_mfa_confirm(uid, code2, cc);
        CHECK(cel_mfa_required_for(uid) == true, "required again after re-enroll");
        CHECK(cel_mfa_reset("m@x.com") == 1, "admin reset removes enrollment");
        CHECK(cel_mfa_required_for(uid) == false, "not required after reset");
    }

    app_db_global_shutdown();
    unlink(path);
    { char x[300]; snprintf(x,sizeof x,"%s-wal",path); unlink(x); snprintf(x,sizeof x,"%s-shm",path); unlink(x); }

    if (failures) { fprintf(stderr, "\n%d check(s) FAILED\n", failures); return 1; }
    fprintf(stderr, "\nall mfa_sqlite checks passed\n");
    return 0;
}
