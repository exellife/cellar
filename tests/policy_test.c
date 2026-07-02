/* cellar — policy / declarative-roles regression test (no DB).
 *
 * Pins the authorization engine's two behaviours that #50 made config-driven:
 *   1. built-in defaults are byte-for-byte unchanged when no config (or no
 *      "_roles" block) is present — the back-compat guarantee;
 *   2. a "_roles" block defines a custom role vocabulary (rider/driver/…) that
 *      the engine honours, additively, alongside the built-ins.
 * Links policy.c + logger + cjson; cel_auth_verify is stubbed (the tested paths
 * never touch auth), so it needs neither Postgres nor the rest of the engine.
 */
#include "policy.h"
#include "core/auth.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* policy.c references cel_auth_resolve (only from cel_identity_from_token, which
 * this test never calls) — satisfy the linker with a stub. */
int cel_auth_resolve(const char *token, cel_user_t *out) { (void)token; (void)out; return -1; }

static int failures = 0;

static void chk(const char *label, bool got, bool want) {
    if (got == want) { printf("  ok   %-40s\n", label); return; }
    printf("  FAIL %-40s got=%d want=%d\n", label, got, want);
    failures++;
}

static void chk_str(const char *label, const char *got, const char *want) {
    bool ok = (got == NULL && want == NULL) || (got && want && !strcmp(got, want));
    if (ok) { printf("  ok   %-40s\n", label); return; }
    printf("  FAIL %-40s got=%s want=%s\n", label, got ? got : "(null)", want ? want : "(null)");
    failures++;
}

/* Load policy JSON via the real file-parsing path (cel_policy_init). */
static void load_policy(const char *json) {
    cel_policy_cleanup();
    char path[] = "/tmp/cel_pol_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    if (write(fd, json, strlen(json)) < 0) { perror("write"); exit(2); }
    close(fd);
    if (cel_policy_init(path) != 0) { printf("  FAIL policy_init parse error\n"); failures++; }
    unlink(path);
}

int main(void) {
    printf("policy / declarative-roles regression\n");

    /* ---- 1. built-in defaults (no config) — back-compat ---- */
    cel_policy_init(NULL);
    printf("[built-in defaults, unlisted table]\n");
    chk("admin list",      cel_policy_allows("t", CEL_ACT_LIST,   "admin"),  true);
    chk("admin delete",    cel_policy_allows("t", CEL_ACT_DELETE, "admin"),  true);
    chk("editor update",   cel_policy_allows("t", CEL_ACT_UPDATE, "editor"), true);
    chk("editor delete",   cel_policy_allows("t", CEL_ACT_DELETE, "editor"), false);
    chk("viewer get",      cel_policy_allows("t", CEL_ACT_GET,    "viewer"), true);
    chk("viewer create",   cel_policy_allows("t", CEL_ACT_CREATE, "viewer"), false);
    chk("anon list",       cel_policy_allows("t", CEL_ACT_LIST,   "anon"),   false);
    chk("unknown rider",   cel_policy_allows("t", CEL_ACT_LIST,   "rider"),  false);
    chk("platform_admin",  cel_policy_allows("t", CEL_ACT_DELETE, "platform_admin"), true);

    /* ---- 2. declarative custom roles (additive/override) ----
     * "_default":"allow" keeps the permissive fallback for the unlisted table
     * `t` (cellar fails closed otherwise; the dedicated checks are in §7). */
    load_policy(
        "{ \"_default\": \"allow\","
        "  \"_roles\": {"
        "    \"admin\":   { \"superuser\": true },"
        "    \"rider\":   { \"allow\": [\"list\",\"get\",\"create\"] },"
        "    \"driver\":  { \"allow\": [\"list\",\"get\",\"update\"] },"
        "    \"support\": { \"superuser\": true },"
        "    \"editor\":  { \"allow\": [] }"
        "} }");
    printf("[_roles bundle, unlisted table]\n");
    chk("rider list",      cel_policy_allows("t", CEL_ACT_LIST,   "rider"),  true);
    chk("rider create",    cel_policy_allows("t", CEL_ACT_CREATE, "rider"),  true);
    chk("rider update",    cel_policy_allows("t", CEL_ACT_UPDATE, "rider"),  false);
    chk("rider delete",    cel_policy_allows("t", CEL_ACT_DELETE, "rider"),  false);
    chk("driver update",   cel_policy_allows("t", CEL_ACT_UPDATE, "driver"), true);
    chk("driver create",   cel_policy_allows("t", CEL_ACT_CREATE, "driver"), false);
    chk("support (su) del",cel_policy_allows("t", CEL_ACT_DELETE, "support"),true);
    chk("admin (su) del",  cel_policy_allows("t", CEL_ACT_DELETE, "admin"),  true);
    /* override: editor revoked to nothing */
    chk("editor revoked",  cel_policy_allows("t", CEL_ACT_LIST,   "editor"), false);
    /* additive: viewer not listed -> keeps built-in default */
    chk("viewer kept",     cel_policy_allows("t", CEL_ACT_GET,    "viewer"), true);
    /* superuser accessor (#51b): config 'superuser' + the platform_admin floor */
    chk("admin is su",     cel_role_is_superuser("admin"),          true);
    chk("support is su",   cel_role_is_superuser("support"),        true);
    chk("rider not su",    cel_role_is_superuser("rider"),          false);
    chk("platform_admin su",cel_role_is_superuser("platform_admin"),true);

    /* ---- 3. per-table policy + owner_column with custom roles ---- */
    load_policy(
        "{ \"_roles\": { \"rider\": { \"allow\": [\"list\"] } },"
        "  \"trips\": {"
        "    \"list\": { \"roles\": [\"rider\",\"admin\"], \"owner_column\": \"rider_id\" }"
        "} }");
    printf("[per-table policy + owner scoping]\n");
    chk("rider listed-table",  cel_policy_allows("trips", CEL_ACT_LIST, "rider"),  true);
    chk("driver not in array", cel_policy_allows("trips", CEL_ACT_LIST, "driver"), false);
    chk_str("rider owner col",  cel_policy_owner_column("trips", CEL_ACT_LIST, "rider"), "rider_id");
    chk_str("admin no scope",   cel_policy_owner_column("trips", CEL_ACT_LIST, "admin"), NULL);

    /* ---- 4. self-service registration gating (#51) ---- */
    char drole[32];
    printf("[self-register: no config]\n");
    cel_policy_cleanup(); cel_policy_init(NULL);
    chk("admin not self-reg",  cel_role_can_self_register("admin"),  false);
    chk("editor not self-reg", cel_role_can_self_register("editor"), false);
    chk("no default signup",   cel_role_default_signup(drole, sizeof drole), false);

    printf("[self-register: _roles bundle]\n");
    load_policy(
        "{ \"_roles\": {"
        "    \"admin\":  { \"superuser\": true, \"self_register\": true },"
        "    \"rider\":  { \"allow\": [\"list\"], \"self_register\": true },"
        "    \"driver\": { \"allow\": [\"list\"], \"self_register\": true },"
        "    \"viewer\": { \"allow\": [\"get\"] }"
        "} }");
    chk("rider self-reg",      cel_role_can_self_register("rider"),  true);
    chk("driver self-reg",     cel_role_can_self_register("driver"), true);
    chk("admin blocked (su)",  cel_role_can_self_register("admin"),  false);
    chk("viewer not flagged",  cel_role_can_self_register("viewer"), false);
    chk("unknown role",        cel_role_can_self_register("ghost"),  false);
    /* default = first self-registerable in config order (admin is skipped: superuser) */
    chk("has default signup",  cel_role_default_signup(drole, sizeof drole), true);
    chk_str("default is rider", drole, "rider");

    /* ---- 5. owner-scope kinds (#52): EQ / OR(any) / VIA ---- */
    printf("[owner scope: EQ / OR / VIA]\n");
    load_policy(
        "{ \"trips\": {"
        "    \"list\":   { \"roles\": [\"rider\"], \"owner_any\": [\"rider_id\",\"driver_id\"] },"
        "    \"create\": { \"roles\": [\"rider\"], \"owner_column\": \"rider_id\" }"
        "  },"
        "  \"messages\": {"
        "    \"list\": { \"roles\": [\"rider\"],"
        "      \"owner_via\": { \"table\":\"trip_parts\", \"ref\":\"trip_id\","
        "                      \"local\":\"trip_id\", \"user\":\"user_id\" } }"
        "} }");
    cel_owner_spec_t os;

    chk("eq present",  cel_policy_owner_scope("trips", CEL_ACT_CREATE, "rider", &os), true);
    chk("eq kind",     os.kind == CEL_OWNER_EQ, true);
    chk_str("eq col",  os.column, "rider_id");

    chk("or present",  cel_policy_owner_scope("trips", CEL_ACT_LIST, "rider", &os), true);
    chk("or kind",     os.kind == CEL_OWNER_ANY, true);
    chk("or ncols 2",  os.ncolumns == 2, true);
    chk_str("or c0",   os.columns[0], "rider_id");
    chk_str("or c1",   os.columns[1], "driver_id");

    chk("via present", cel_policy_owner_scope("messages", CEL_ACT_LIST, "rider", &os), true);
    chk("via kind",    os.kind == CEL_OWNER_VIA, true);
    chk_str("via tbl", os.via.table, "trip_parts");
    chk_str("via ref", os.via.ref,   "trip_id");
    chk_str("via loc", os.via.local, "trip_id");
    chk_str("via usr", os.via.user,  "user_id");

    /* superuser is never row-scoped, regardless of config */
    chk("admin no scope", cel_policy_owner_scope("trips", CEL_ACT_LIST, "admin", &os), false);
    /* unscoped action -> none */
    chk("delete unscoped", cel_policy_owner_scope("trips", CEL_ACT_DELETE, "rider", &os), false);

    /* ---- 6. RPC whitelist authz (#54) ---- */
    printf("[rpc whitelist]\n");
    cel_policy_cleanup(); cel_policy_init(NULL);
    chk("no config -> deny", cel_policy_rpc_allows("request_ride", "admin"), false);

    load_policy(
        "{ \"_roles\": { \"admin\": { \"superuser\": true } },"
        "  \"_rpc\": { \"request_ride\": { \"roles\": [\"rider\", \"admin\"] } } }");
    chk("rider allowed",       cel_policy_rpc_allows("request_ride", "rider"),  true);
    chk("driver denied",       cel_policy_rpc_allows("request_ride", "driver"), false);
    chk("admin (su) allowed",  cel_policy_rpc_allows("request_ride", "admin"),  true);
    chk("anon denied",         cel_policy_rpc_allows("request_ride", "anon"),   false);
    /* a non-whitelisted function is unreachable even by a superuser */
    chk("unlisted fn (su) deny", cel_policy_rpc_allows("pg_sleep", "admin"),    false);

    /* ---- 7. fail-closed default for unlisted tables (H-1) ---- */
    printf("[fail-closed default: unlisted tables]\n");
    /* config loaded, no "_default": an unlisted table denies even the built-in
     * editor/viewer grants (previously it fell open to them). */
    load_policy(
        "{ \"_roles\": { \"editor\": { \"allow\": [\"list\",\"get\",\"create\",\"update\"] },"
        "                \"viewer\": { \"allow\": [\"list\",\"get\"] } },"
        "  \"notes\": { \"list\": [\"editor\",\"viewer\"] } }");
    chk("no _default: editor unlisted deny", cel_policy_allows("other", CEL_ACT_LIST,  "editor"), false);
    chk("no _default: viewer unlisted deny", cel_policy_allows("other", CEL_ACT_GET,   "viewer"), false);
    chk("no _default: listed still works",   cel_policy_allows("notes", CEL_ACT_LIST,  "editor"), true);
    chk("superuser bypasses fail-closed",    cel_policy_allows("other", CEL_ACT_DELETE,"platform_admin"), true);

    /* "_default":"deny" (string) and {"deny":true} (object, the documented form) */
    load_policy("{ \"_default\": \"deny\", \"_roles\": { \"editor\": { \"allow\": [\"list\"] } } }");
    chk("_default deny (string)",            cel_policy_allows("other", CEL_ACT_LIST, "editor"), false);
    load_policy("{ \"_default\": { \"deny\": true }, \"_roles\": { \"editor\": { \"allow\": [\"list\"] } } }");
    chk("_default {deny:true} (object)",     cel_policy_allows("other", CEL_ACT_LIST, "editor"), false);

    /* "_default":"allow" (string) and {"allow":true} (object) opt back into fallback */
    load_policy(
        "{ \"_default\": \"allow\","
        "  \"_roles\": { \"editor\": { \"allow\": [\"list\",\"get\",\"create\",\"update\"] },"
        "                \"viewer\": { \"allow\": [\"list\",\"get\"] } } }");
    chk("_default allow: editor fallback",   cel_policy_allows("other", CEL_ACT_UPDATE, "editor"), true);
    chk("_default allow: viewer fallback",   cel_policy_allows("other", CEL_ACT_GET,    "viewer"), true);
    chk("_default allow: viewer no-write",   cel_policy_allows("other", CEL_ACT_CREATE, "viewer"), false);
    load_policy("{ \"_default\": { \"allow\": true }, \"_roles\": { \"viewer\": { \"allow\": [\"get\"] } } }");
    chk("_default {allow:true} (object)",    cel_policy_allows("other", CEL_ACT_GET, "viewer"), true);

    /* ---- 8. meta-only table entry (realtime) must NOT fail-close CRUD ----
     * Footgun fix: a table entry that lists NO action (just metadata like
     * "realtime": true) falls through to "_default" like an unlisted table,
     * instead of silently denying every action. Once it lists any action it's an
     * explicit allow-list again (unlisted actions deny — H-1 preserved). */
    printf("[meta-only table entry (realtime) does not fail-close]\n");
    load_policy(
        "{ \"_default\": \"allow\","
        "  \"_roles\": { \"staff\": { \"allow\": [\"list\",\"get\",\"create\",\"update\",\"delete\"] } },"
        "  \"live\": { \"realtime\": true } }");
    chk("realtime-only: staff list (-> default)",   cel_policy_allows("live", CEL_ACT_LIST,   "staff"), true);
    chk("realtime-only: staff create (-> default)", cel_policy_allows("live", CEL_ACT_CREATE, "staff"), true);
    chk("realtime-only: staff delete (-> default)", cel_policy_allows("live", CEL_ACT_DELETE, "staff"), true);
    chk("realtime-only: realtime still enabled",    cel_policy_realtime_enabled("live"), true);

    /* once it lists an action, unlisted actions deny again (explicit allow-list) */
    load_policy(
        "{ \"_default\": \"allow\","
        "  \"_roles\": { \"staff\": { \"allow\": [\"list\",\"get\",\"create\",\"update\",\"delete\"] } },"
        "  \"live\": { \"realtime\": true, \"list\": [\"staff\"] } }");
    chk("realtime+list: list allowed",              cel_policy_allows("live", CEL_ACT_LIST,   "staff"), true);
    chk("realtime+list: create denied (explicit)",  cel_policy_allows("live", CEL_ACT_CREATE, "staff"), false);

    /* meta-only under a fail-closed default (no "_default") still denies — safe */
    load_policy(
        "{ \"_roles\": { \"staff\": { \"allow\": [\"list\"] } },"
        "  \"live\": { \"realtime\": true } }");
    chk("realtime-only, no _default: deny",         cel_policy_allows("live", CEL_ACT_LIST, "staff"), false);
    chk("realtime-only, no _default: superuser ok", cel_policy_allows("live", CEL_ACT_LIST, "platform_admin"), true);

    /* ---- 9. per-app session policy (_session) resolution ---- */
    printf("[_session policy resolution]\n");
    cel_session_policy_t sp;

    /* no config -> built-in default (fixed, 24h, no cap) — the back-compat guarantee */
    cel_policy_cleanup(); cel_policy_init(NULL);
    cel_policy_session(&sp);
    chk("default strategy fixed", sp.strategy == CEL_SESSION_FIXED, true);
    chk("default ttl 24h",        sp.ttl_seconds == 24 * 3600, true);
    chk("default no cap",         sp.absolute_max_seconds == 0, true);

    /* explicit fixed with a custom lifetime */
    load_policy("{ \"_session\": { \"strategy\": \"fixed\", \"ttl_seconds\": 3600 } }");
    cel_policy_session(&sp);
    chk("fixed strategy",   sp.strategy == CEL_SESSION_FIXED, true);
    chk("fixed custom ttl", sp.ttl_seconds == 3600, true);

    /* sliding with explicit idle window + absolute cap */
    load_policy("{ \"_session\": { \"strategy\": \"sliding\", \"ttl_seconds\": 900, \"absolute_max_seconds\": 43200 } }");
    cel_policy_session(&sp);
    chk("sliding strategy", sp.strategy == CEL_SESSION_SLIDING, true);
    chk("sliding idle 900", sp.ttl_seconds == 900, true);
    chk("sliding cap 12h",  sp.absolute_max_seconds == 43200, true);

    /* sliding without ttl -> 1h idle default, no cap */
    load_policy("{ \"_session\": { \"strategy\": \"sliding\" } }");
    cel_policy_session(&sp);
    chk("sliding default idle 1h", sp.ttl_seconds == 3600, true);
    chk("sliding default no cap",  sp.absolute_max_seconds == 0, true);

    /* unknown strategy -> safe default (fixed); other parsed fields still applied */
    load_policy("{ \"_session\": { \"strategy\": \"bogus\", \"ttl_seconds\": 100 } }");
    cel_policy_session(&sp);
    chk("unknown -> fixed",      sp.strategy == CEL_SESSION_FIXED, true);
    chk("unknown keeps ttl",     sp.ttl_seconds == 100, true);

    /* non-positive ttl ignored (keeps the default), negative cap ignored */
    load_policy("{ \"_session\": { \"strategy\": \"fixed\", \"ttl_seconds\": 0, \"absolute_max_seconds\": -5 } }");
    cel_policy_session(&sp);
    chk("ttl<=0 ignored -> 24h",  sp.ttl_seconds == 24 * 3600, true);
    chk("cap<0 ignored -> 0",     sp.absolute_max_seconds == 0, true);

    /* huge values clamp to INT_MAX (no UB / negative overflow from the double->int cast) */
    load_policy("{ \"_session\": { \"strategy\": \"sliding\", \"ttl_seconds\": 3000000000, \"absolute_max_seconds\": 5000000000 } }");
    cel_policy_session(&sp);
    chk("huge ttl clamped to INT_MAX", sp.ttl_seconds == INT_MAX, true);
    chk("huge ttl stays positive",     sp.ttl_seconds > 0, true);
    chk("huge cap clamped to INT_MAX", sp.absolute_max_seconds == INT_MAX, true);

    /* ---- per-app OAuth client id (bundle _oauth) ---- */
    printf("per-app _oauth client id\n");
    load_policy("{ \"_oauth\": { \"google\": { \"client_id\": \"app-123.apps.googleusercontent.com\" } } }");
    chk_str("google client id from bundle", cel_policy_oauth_client_id("google"),
            "app-123.apps.googleusercontent.com");
    chk_str("unset provider -> NULL (fall back to env)", cel_policy_oauth_client_id("apple"), NULL);

    load_policy("{ \"_default\": \"deny\" }");   /* no _oauth at all */
    chk_str("no _oauth -> NULL (fall back to env)", cel_policy_oauth_client_id("google"), NULL);

    /* malformed shapes must not crash and must yield NULL (fail back to env) */
    load_policy("{ \"_oauth\": { \"google\": { \"client_id\": 42 } } }");
    chk_str("non-string client_id -> NULL", cel_policy_oauth_client_id("google"), NULL);
    load_policy("{ \"_oauth\": { \"google\": \"nope\" } }");
    chk_str("provider not an object -> NULL", cel_policy_oauth_client_id("google"), NULL);
    load_policy("{ \"_oauth\": { \"google\": { \"client_id\": \"\" } } }");
    chk_str("empty client_id -> NULL", cel_policy_oauth_client_id("google"), NULL);

    /* ---- per-app email From (bundle _mail) ---- */
    printf("per-app _mail From\n");
    load_policy("{ \"_mail\": { \"from\": \"no-reply@jarchy.com\", \"from_name\": \"Jarchy\" } }");
    chk_str("mail from from bundle",      cel_policy_mail_from(),      "no-reply@jarchy.com");
    chk_str("mail from_name from bundle", cel_policy_mail_from_name(), "Jarchy");

    load_policy("{ \"_mail\": { \"from\": \"only-addr@x.com\" } }");   /* from without from_name */
    chk_str("from set, name unset -> NULL", cel_policy_mail_from_name(), NULL);
    chk_str("from still resolves",          cel_policy_mail_from(),      "only-addr@x.com");

    load_policy("{ \"_default\": \"deny\" }");   /* no _mail */
    chk_str("no _mail from -> NULL",  cel_policy_mail_from(),      NULL);
    chk_str("no _mail name -> NULL",  cel_policy_mail_from_name(), NULL);

    load_policy("{ \"_mail\": { \"from\": 7, \"from_name\": \"\" } }");  /* malformed / empty */
    chk_str("non-string from -> NULL", cel_policy_mail_from(),      NULL);
    chk_str("empty from_name -> NULL", cel_policy_mail_from_name(), NULL);

    cel_policy_cleanup();
    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
