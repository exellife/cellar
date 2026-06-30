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

static void count_devices_cb(void *ctx, const char *id, const char *label,
                             long created_at, long last_used_at, long expires_at) {
    (void)id; (void)label; (void)created_at; (void)last_used_at; (void)expires_at;
    (*(int *)ctx)++;
}

static void count_pushes_cb(void *ctx, const char *id, const char *endpoint, const char *ua,
                            long created_at, long last_used_at) {
    (void)id; (void)endpoint; (void)ua; (void)created_at; (void)last_used_at;
    (*(int *)ctx)++;
}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok: %s\n", msg); } \
} while (0)

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
    int rc = cel_auth_register("a@x.com", "password1", "admin", tok, sizeof tok, &u);
    CHECK(rc == CEL_AUTH_OK, "register ok");
    CHECK(strlen(u.id) == 36, "register minted a uuid");
    CHECK(strcmp(u.email, "a@x.com") == 0, "register email");
    char first_id[37]; snprintf(first_id, sizeof first_id, "%s", u.id);

    /* verify the session token from register */
    rc = cel_auth_verify(tok, &u);
    CHECK(rc == CEL_AUTH_OK && strcmp(u.id, first_id) == 0, "verify register session");

    /* duplicate email -> conflict */
    rc = cel_auth_register("a@x.com", "password1", "admin", tok, sizeof tok, &u);
    CHECK(rc == CEL_AUTH_CONFLICT, "duplicate register -> conflict");

    /* login: right / wrong password / unknown user */
    rc = cel_auth_login("a@x.com", "password1", tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_OK, "login ok");
    rc = cel_auth_login("a@x.com", "WRONG", tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_INVALID, "login wrong password -> invalid");
    rc = cel_auth_login("ghost@x.com", "password1", tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_INVALID, "login unknown user -> invalid");

    /* a fresh login token verifies, then logout revokes it */
    rc = cel_auth_login("a@x.com", "password1", tok, sizeof tok, chal, sizeof chal, &u);
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
    rc = cel_auth_login("a@x.com", "newpassword2", tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_OK, "login with new password");
    rc = cel_auth_login("a@x.com", "password1", tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_INVALID, "old password rejected after reset");

    /* admin-provisioned account + duplicate */
    char id2[37];
    rc = cel_auth_create_user("b@x.com", "password1", "editor", id2, sizeof id2);
    CHECK(rc == CEL_AUTH_OK, "create_user ok");
    CHECK(cel_auth_create_user("b@x.com", "password1", "editor", id2, sizeof id2) == CEL_AUTH_CONFLICT,
          "create_user duplicate -> conflict");
    rc = cel_auth_login("b@x.com", "password1", tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_OK && strcmp(u.role, "editor") == 0, "created user can log in");

    /* seed (upsert): re-seed updates the password, login uses the latest */
    CHECK(cel_auth_seed_admin("root@x.com", "rootpass1") == 0, "seed admin");
    CHECK(cel_auth_seed_user("root@x.com", "rootpass2", "admin") == 0, "re-seed (upsert) admin");
    rc = cel_auth_login("root@x.com", "rootpass2", tok, sizeof tok, chal, sizeof chal, &u);
    CHECK(rc == CEL_AUTH_OK && strcmp(u.role, "admin") == 0, "seeded admin logs in with new password");

    /* ---- device tokens (PIN fast-login) ---- */
    char dtok[129], did[37], stok[129];
    cel_user_t du;
    rc = cel_auth_device_create(id2, "phone", 3600, dtok, sizeof dtok, did, sizeof did);
    CHECK(rc == CEL_AUTH_OK && strlen(did) == 36, "device_create -> token + id");
    rc = cel_auth_device_exchange(dtok, stok, sizeof stok, &du);
    CHECK(rc == CEL_AUTH_OK && strcmp(du.id, id2) == 0, "device_exchange mints a session for the owner");
    CHECK(cel_auth_verify(stok, &du) == CEL_AUTH_OK, "exchanged session token is valid");
    CHECK(cel_auth_device_exchange(dtok, stok, sizeof stok, &du) == CEL_AUTH_OK, "device token is reusable");

    int dcount = 0; cel_auth_device_list(id2, count_devices_cb, &dcount);
    CHECK(dcount == 1, "device_list counts the active device");

    /* a user can only revoke their own device */
    CHECK(cel_auth_device_revoke(first_id, did) == CEL_AUTH_INVALID, "cannot revoke another user's device");
    CHECK(cel_auth_device_revoke(id2, did) == CEL_AUTH_OK, "device_revoke (own) ok");
    CHECK(cel_auth_device_exchange(dtok, stok, sizeof stok, &du) == CEL_AUTH_INVALID, "revoked device rejected");
    dcount = 0; cel_auth_device_list(id2, count_devices_cb, &dcount);
    CHECK(dcount == 0, "device_list excludes the revoked device");

    /* tie-in: an admin password reset (set_password) revokes the user's devices */
    char dtok2[129], did2[37];
    CHECK(cel_auth_device_create(id2, "tablet", 3600, dtok2, sizeof dtok2, did2, sizeof did2) == CEL_AUTH_OK,
          "second device for the reset test");
    CHECK(cel_auth_device_exchange(dtok2, stok, sizeof stok, &du) == CEL_AUTH_OK, "device works before reset");
    CHECK(cel_auth_set_password("b@x.com", "newpass-b1") == CEL_AUTH_OK, "set_password (admin reset)");
    CHECK(cel_auth_device_exchange(dtok2, stok, sizeof stok, &du) == CEL_AUTH_INVALID,
          "set_password revoked the device token");

    /* tie-in: revoke_user_sessions ("log out everywhere") also kills device tokens */
    char dtok3[129], did3[37];
    CHECK(cel_auth_device_create(id2, "laptop", 3600, dtok3, sizeof dtok3, did3, sizeof did3) == CEL_AUTH_OK,
          "third device for the revoke-everywhere test");
    cel_auth_revoke_user_sessions("b@x.com");
    CHECK(cel_auth_device_exchange(dtok3, stok, sizeof stok, &du) == CEL_AUTH_INVALID,
          "revoke_user_sessions killed the device token");

    /* a bad token never exchanges */
    CHECK(cel_auth_device_exchange("not-a-real-token", stok, sizeof stok, &du) == CEL_AUTH_INVALID,
          "garbage device token rejected");

    /* Migration regression: an ALREADY-PROVISIONED db at an older user_version must
     * still gain cel_device_tokens on re-apply (the table was first shipped without a
     * version bump → existing dbs 500'd on the feature). Simulate a v2 db missing it. */
    {
        sqlite3 *mc = app_db_conn_acquire(app);
        CHECK(sqlite3_exec(mc, "DROP TABLE cel_device_tokens; PRAGMA user_version = 2;",
                           NULL, NULL, NULL) == SQLITE_OK, "simulate a v2 db without the device table");
        CHECK(cel_auth_schema_apply(mc) == 0, "re-apply schema upgrades v2 -> current");
        app_db_conn_release(app, mc);
    }
    char dtok5[129], did5[37];
    CHECK(cel_auth_device_create(id2, "after-upgrade", 3600, dtok5, sizeof dtok5, did5, sizeof did5) == CEL_AUTH_OK,
          "device table present again after the version upgrade");

    /* Same migration check for cel_push_subscriptions (v3 -> v4). */
    {
        sqlite3 *mc = app_db_conn_acquire(app);
        CHECK(sqlite3_exec(mc, "DROP TABLE cel_push_subscriptions; PRAGMA user_version = 3;",
                           NULL, NULL, NULL) == SQLITE_OK, "simulate a v3 db without the push table");
        CHECK(cel_auth_schema_apply(mc) == 0, "re-apply schema upgrades v3 -> current");
        app_db_conn_release(app, mc);
    }
    char psid[37];
    CHECK(cel_auth_push_subscribe(id2, "https://push.example/ep1", "p256key", "authkey", "ua",
                                  psid, sizeof psid) == CEL_AUTH_OK,
          "push table present again after the version upgrade");

    /* Push subscription CRUD: upsert by endpoint, list, unsubscribe. */
    char psid2[37];
    CHECK(cel_auth_push_subscribe(id2, "https://push.example/ep1", "p256key2", "authkey2", "ua2",
                                  psid2, sizeof psid2) == CEL_AUTH_OK && strcmp(psid, psid2) == 0,
          "re-subscribe same endpoint upserts (stable id)");
    int pcount = 0; cel_auth_push_list(id2, count_pushes_cb, &pcount);
    CHECK(pcount == 1, "push_list shows one subscription after upsert");
    CHECK(cel_auth_push_unsubscribe(id2, "https://push.example/ep1") == CEL_AUTH_OK, "unsubscribe by endpoint");
    CHECK(cel_auth_push_unsubscribe(id2, "https://push.example/ep1") == CEL_AUTH_INVALID, "unsubscribe again -> none");
    pcount = 0; cel_auth_push_list(id2, count_pushes_cb, &pcount);
    CHECK(pcount == 0, "push_list empty after unsubscribe");

    app_db_global_shutdown();
    unlink(path);
    { char x[300]; snprintf(x,sizeof x,"%s-wal",path); unlink(x); snprintf(x,sizeof x,"%s-shm",path); unlink(x); }

    if (failures) { fprintf(stderr, "\n%d check(s) FAILED\n", failures); return 1; }
    fprintf(stderr, "\nall auth_sqlite checks passed\n");
    return 0;
}
