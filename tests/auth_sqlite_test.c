/* ============================================================================
 * auth_sqlite_test — the identity layer on per-app SQLite.
 *
 * Exercises register / login / verify / logout / password-reset / create-user /
 * seed against a throwaway SQLite app (auth schema applied in-process). MFA is
 * stubbed off (its conversion is the next chunk), so the login path never needs
 * Postgres. libpq-free.
 * ============================================================================ */
#include "auth.h"
#include "auth_schema.h"
#include "app_db.h"
#include "password.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* MFA is off in this test — stub the two calls auth makes (no Postgres). */
bool cel_mfa_required_for(const char *user_id) { (void)user_id; return false; }
int  cel_mfa_create_challenge(const char *user_id, char *out, size_t n) {
    (void)user_id; (void)out; (void)n; return -1;
}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok: %s\n", msg); } \
} while (0)

#define TTL 3600

int main(void) {
    char path[256];
    snprintf(path, sizeof path, "/tmp/cellar_auth_%d.db", (int)getpid());
    unlink(path);
    { char x[300]; snprintf(x,sizeof x,"%s-wal",path); unlink(x); snprintf(x,sizeof x,"%s-shm",path); unlink(x); }

    CHECK(cel_crypto_init() == 0, "crypto init");
    cel_auth_init();                          /* decoy hash for timing equalization */

    app_db_global_init();
    app_db_t *app = app_db_get(path);
    CHECK(app != NULL, "open app");
    sqlite3 *sc = app_db_conn_acquire(app);
    CHECK(cel_auth_schema_apply(sc) == 0, "apply auth schema");
    app_db_conn_release(app, sc);
    app_db_set_current(app);

    char tok[129], chal[129];
    cel_user_t u;

    /* register -> token + user */
    int rc = cel_auth_register("a@x.com", "password1", "admin", TTL, tok, sizeof tok, &u);
    CHECK(rc == CEL_AUTH_OK, "register ok");
    CHECK(strlen(u.id) == 36, "register minted a uuid");
    CHECK(strcmp(u.email, "a@x.com") == 0, "register email");
    char first_id[37]; snprintf(first_id, sizeof first_id, "%s", u.id);

    /* verify the session token from register */
    rc = cel_auth_verify(tok, &u);
    CHECK(rc == CEL_AUTH_OK && strcmp(u.id, first_id) == 0, "verify register session");

    /* duplicate email -> conflict */
    rc = cel_auth_register("a@x.com", "password1", "admin", TTL, tok, sizeof tok, &u);
    CHECK(rc == CEL_AUTH_CONFLICT, "duplicate register -> conflict");

    /* login: right / wrong password / unknown user */
    rc = cel_auth_login("a@x.com", "password1", TTL, tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_OK, "login ok");
    rc = cel_auth_login("a@x.com", "WRONG", TTL, tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_INVALID, "login wrong password -> invalid");
    rc = cel_auth_login("ghost@x.com", "password1", TTL, tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_INVALID, "login unknown user -> invalid");

    /* a fresh login token verifies, then logout revokes it */
    rc = cel_auth_login("a@x.com", "password1", TTL, tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_OK, "login for logout test");
    CHECK(cel_auth_verify(tok, &u) == CEL_AUTH_OK, "token valid before logout");
    CHECK(cel_auth_logout(tok) == CEL_AUTH_OK, "logout ok");
    CHECK(cel_auth_verify(tok, &u) == CEL_AUTH_INVALID, "token invalid after logout");

    /* password reset: issue -> redeem -> new password works, old fails */
    char rtok[129];
    rc = cel_auth_create_password_reset("a@x.com", rtok, sizeof rtok);
    CHECK(rc == CEL_AUTH_OK, "create password reset");
    rc = cel_auth_perform_password_reset(rtok, "newpassword2");
    CHECK(rc == CEL_AUTH_OK, "perform password reset");
    CHECK(cel_auth_perform_password_reset(rtok, "again3") == CEL_AUTH_INVALID, "reset token single-use");
    rc = cel_auth_login("a@x.com", "newpassword2", TTL, tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_OK, "login with new password");
    rc = cel_auth_login("a@x.com", "password1", TTL, tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_INVALID, "old password rejected after reset");

    /* admin-provisioned account + duplicate */
    char id2[37];
    rc = cel_auth_create_user("b@x.com", "password1", "editor", "", id2, sizeof id2);
    CHECK(rc == CEL_AUTH_OK, "create_user ok");
    CHECK(cel_auth_create_user("b@x.com", "password1", "editor", "", id2, sizeof id2) == CEL_AUTH_CONFLICT,
          "create_user duplicate -> conflict");
    rc = cel_auth_login("b@x.com", "password1", TTL, tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_OK && strcmp(u.role, "editor") == 0, "created user can log in");

    /* seed (upsert): re-seed updates the password, login uses the latest */
    CHECK(cel_auth_seed_admin("root@x.com", "rootpass1") == 0, "seed admin");
    CHECK(cel_auth_seed_user("root@x.com", "rootpass2", "admin") == 0, "re-seed (upsert) admin");
    rc = cel_auth_login("root@x.com", "rootpass2", TTL, tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_OK && strcmp(u.role, "admin") == 0, "seeded admin logs in with new password");

    app_db_global_shutdown();
    unlink(path);
    { char x[300]; snprintf(x,sizeof x,"%s-wal",path); unlink(x); snprintf(x,sizeof x,"%s-shm",path); unlink(x); }

    if (failures) { fprintf(stderr, "\n%d check(s) FAILED\n", failures); return 1; }
    fprintf(stderr, "\nall auth_sqlite checks passed\n");
    return 0;
}
