/* pgforge — policy / declarative-roles regression test (no DB).
 *
 * Pins the authorization engine's two behaviours that #50 made config-driven:
 *   1. built-in defaults are byte-for-byte unchanged when no config (or no
 *      "_roles" block) is present — the back-compat guarantee;
 *   2. a "_roles" block defines a custom role vocabulary (rider/driver/…) that
 *      the engine honours, additively, alongside the built-ins.
 * Links policy.c + logger + cjson; pgf_auth_verify is stubbed (the tested paths
 * never touch auth), so it needs neither Postgres nor the rest of the engine.
 */
#include "policy.h"
#include "core/auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* policy.c references pgf_auth_resolve (only from pgf_identity_from_token, which
 * this test never calls) — satisfy the linker with a stub. */
int pgf_auth_resolve(const char *token, pgf_user_t *out) { (void)token; (void)out; return -1; }

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

/* Load policy JSON via the real file-parsing path (pgf_policy_init). */
static void load_policy(const char *json) {
    pgf_policy_cleanup();
    char path[] = "/tmp/pgf_pol_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    if (write(fd, json, strlen(json)) < 0) { perror("write"); exit(2); }
    close(fd);
    if (pgf_policy_init(path) != 0) { printf("  FAIL policy_init parse error\n"); failures++; }
    unlink(path);
}

int main(void) {
    printf("policy / declarative-roles regression\n");

    /* ---- 1. built-in defaults (no config) — back-compat ---- */
    pgf_policy_init(NULL);
    printf("[built-in defaults, unlisted table]\n");
    chk("admin list",      pgf_policy_allows("t", PGF_ACT_LIST,   "admin"),  true);
    chk("admin delete",    pgf_policy_allows("t", PGF_ACT_DELETE, "admin"),  true);
    chk("editor update",   pgf_policy_allows("t", PGF_ACT_UPDATE, "editor"), true);
    chk("editor delete",   pgf_policy_allows("t", PGF_ACT_DELETE, "editor"), false);
    chk("viewer get",      pgf_policy_allows("t", PGF_ACT_GET,    "viewer"), true);
    chk("viewer create",   pgf_policy_allows("t", PGF_ACT_CREATE, "viewer"), false);
    chk("anon list",       pgf_policy_allows("t", PGF_ACT_LIST,   "anon"),   false);
    chk("unknown rider",   pgf_policy_allows("t", PGF_ACT_LIST,   "rider"),  false);
    chk("platform_admin",  pgf_policy_allows("t", PGF_ACT_DELETE, "platform_admin"), true);

    /* ---- 2. declarative custom roles (additive/override) ----
     * "_default":"allow" keeps the permissive fallback for the unlisted table
     * `t` (pgforge fails closed otherwise; the dedicated checks are in §7). */
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
    chk("rider list",      pgf_policy_allows("t", PGF_ACT_LIST,   "rider"),  true);
    chk("rider create",    pgf_policy_allows("t", PGF_ACT_CREATE, "rider"),  true);
    chk("rider update",    pgf_policy_allows("t", PGF_ACT_UPDATE, "rider"),  false);
    chk("rider delete",    pgf_policy_allows("t", PGF_ACT_DELETE, "rider"),  false);
    chk("driver update",   pgf_policy_allows("t", PGF_ACT_UPDATE, "driver"), true);
    chk("driver create",   pgf_policy_allows("t", PGF_ACT_CREATE, "driver"), false);
    chk("support (su) del",pgf_policy_allows("t", PGF_ACT_DELETE, "support"),true);
    chk("admin (su) del",  pgf_policy_allows("t", PGF_ACT_DELETE, "admin"),  true);
    /* override: editor revoked to nothing */
    chk("editor revoked",  pgf_policy_allows("t", PGF_ACT_LIST,   "editor"), false);
    /* additive: viewer not listed -> keeps built-in default */
    chk("viewer kept",     pgf_policy_allows("t", PGF_ACT_GET,    "viewer"), true);
    /* superuser accessor (#51b): config 'superuser' + the platform_admin floor */
    chk("admin is su",     pgf_role_is_superuser("admin"),          true);
    chk("support is su",   pgf_role_is_superuser("support"),        true);
    chk("rider not su",    pgf_role_is_superuser("rider"),          false);
    chk("platform_admin su",pgf_role_is_superuser("platform_admin"),true);

    /* ---- 3. per-table policy + owner_column with custom roles ---- */
    load_policy(
        "{ \"_roles\": { \"rider\": { \"allow\": [\"list\"] } },"
        "  \"trips\": {"
        "    \"list\": { \"roles\": [\"rider\",\"admin\"], \"owner_column\": \"rider_id\" }"
        "} }");
    printf("[per-table policy + owner scoping]\n");
    chk("rider listed-table",  pgf_policy_allows("trips", PGF_ACT_LIST, "rider"),  true);
    chk("driver not in array", pgf_policy_allows("trips", PGF_ACT_LIST, "driver"), false);
    chk_str("rider owner col",  pgf_policy_owner_column("trips", PGF_ACT_LIST, "rider"), "rider_id");
    chk_str("admin no scope",   pgf_policy_owner_column("trips", PGF_ACT_LIST, "admin"), NULL);

    /* ---- 4. self-service registration gating (#51) ---- */
    char drole[32];
    printf("[self-register: no config]\n");
    pgf_policy_cleanup(); pgf_policy_init(NULL);
    chk("admin not self-reg",  pgf_role_can_self_register("admin"),  false);
    chk("editor not self-reg", pgf_role_can_self_register("editor"), false);
    chk("no default signup",   pgf_role_default_signup(drole, sizeof drole), false);

    printf("[self-register: _roles bundle]\n");
    load_policy(
        "{ \"_roles\": {"
        "    \"admin\":  { \"superuser\": true, \"self_register\": true },"
        "    \"rider\":  { \"allow\": [\"list\"], \"self_register\": true },"
        "    \"driver\": { \"allow\": [\"list\"], \"self_register\": true },"
        "    \"viewer\": { \"allow\": [\"get\"] }"
        "} }");
    chk("rider self-reg",      pgf_role_can_self_register("rider"),  true);
    chk("driver self-reg",     pgf_role_can_self_register("driver"), true);
    chk("admin blocked (su)",  pgf_role_can_self_register("admin"),  false);
    chk("viewer not flagged",  pgf_role_can_self_register("viewer"), false);
    chk("unknown role",        pgf_role_can_self_register("ghost"),  false);
    /* default = first self-registerable in config order (admin is skipped: superuser) */
    chk("has default signup",  pgf_role_default_signup(drole, sizeof drole), true);
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
    pgf_owner_spec_t os;

    chk("eq present",  pgf_policy_owner_scope("trips", PGF_ACT_CREATE, "rider", &os), true);
    chk("eq kind",     os.kind == PGF_OWNER_EQ, true);
    chk_str("eq col",  os.column, "rider_id");

    chk("or present",  pgf_policy_owner_scope("trips", PGF_ACT_LIST, "rider", &os), true);
    chk("or kind",     os.kind == PGF_OWNER_ANY, true);
    chk("or ncols 2",  os.ncolumns == 2, true);
    chk_str("or c0",   os.columns[0], "rider_id");
    chk_str("or c1",   os.columns[1], "driver_id");

    chk("via present", pgf_policy_owner_scope("messages", PGF_ACT_LIST, "rider", &os), true);
    chk("via kind",    os.kind == PGF_OWNER_VIA, true);
    chk_str("via tbl", os.via.table, "trip_parts");
    chk_str("via ref", os.via.ref,   "trip_id");
    chk_str("via loc", os.via.local, "trip_id");
    chk_str("via usr", os.via.user,  "user_id");

    /* superuser is never row-scoped, regardless of config */
    chk("admin no scope", pgf_policy_owner_scope("trips", PGF_ACT_LIST, "admin", &os), false);
    /* unscoped action -> none */
    chk("delete unscoped", pgf_policy_owner_scope("trips", PGF_ACT_DELETE, "rider", &os), false);

    /* ---- 6. RPC whitelist authz (#54) ---- */
    printf("[rpc whitelist]\n");
    pgf_policy_cleanup(); pgf_policy_init(NULL);
    chk("no config -> deny", pgf_policy_rpc_allows("request_ride", "admin"), false);

    load_policy(
        "{ \"_roles\": { \"admin\": { \"superuser\": true } },"
        "  \"_rpc\": { \"request_ride\": { \"roles\": [\"rider\", \"admin\"] } } }");
    chk("rider allowed",       pgf_policy_rpc_allows("request_ride", "rider"),  true);
    chk("driver denied",       pgf_policy_rpc_allows("request_ride", "driver"), false);
    chk("admin (su) allowed",  pgf_policy_rpc_allows("request_ride", "admin"),  true);
    chk("anon denied",         pgf_policy_rpc_allows("request_ride", "anon"),   false);
    /* a non-whitelisted function is unreachable even by a superuser */
    chk("unlisted fn (su) deny", pgf_policy_rpc_allows("pg_sleep", "admin"),    false);

    /* ---- 7. fail-closed default for unlisted tables (H-1) ---- */
    printf("[fail-closed default: unlisted tables]\n");
    /* config loaded, no "_default": an unlisted table denies even the built-in
     * editor/viewer grants (previously it fell open to them). */
    load_policy(
        "{ \"_roles\": { \"editor\": { \"allow\": [\"list\",\"get\",\"create\",\"update\"] },"
        "                \"viewer\": { \"allow\": [\"list\",\"get\"] } },"
        "  \"notes\": { \"list\": [\"editor\",\"viewer\"] } }");
    chk("no _default: editor unlisted deny", pgf_policy_allows("other", PGF_ACT_LIST,  "editor"), false);
    chk("no _default: viewer unlisted deny", pgf_policy_allows("other", PGF_ACT_GET,   "viewer"), false);
    chk("no _default: listed still works",   pgf_policy_allows("notes", PGF_ACT_LIST,  "editor"), true);
    chk("superuser bypasses fail-closed",    pgf_policy_allows("other", PGF_ACT_DELETE,"platform_admin"), true);

    /* "_default":"deny" (string) and {"deny":true} (object, the documented form) */
    load_policy("{ \"_default\": \"deny\", \"_roles\": { \"editor\": { \"allow\": [\"list\"] } } }");
    chk("_default deny (string)",            pgf_policy_allows("other", PGF_ACT_LIST, "editor"), false);
    load_policy("{ \"_default\": { \"deny\": true }, \"_roles\": { \"editor\": { \"allow\": [\"list\"] } } }");
    chk("_default {deny:true} (object)",     pgf_policy_allows("other", PGF_ACT_LIST, "editor"), false);

    /* "_default":"allow" (string) and {"allow":true} (object) opt back into fallback */
    load_policy(
        "{ \"_default\": \"allow\","
        "  \"_roles\": { \"editor\": { \"allow\": [\"list\",\"get\",\"create\",\"update\"] },"
        "                \"viewer\": { \"allow\": [\"list\",\"get\"] } } }");
    chk("_default allow: editor fallback",   pgf_policy_allows("other", PGF_ACT_UPDATE, "editor"), true);
    chk("_default allow: viewer fallback",   pgf_policy_allows("other", PGF_ACT_GET,    "viewer"), true);
    chk("_default allow: viewer no-write",   pgf_policy_allows("other", PGF_ACT_CREATE, "viewer"), false);
    load_policy("{ \"_default\": { \"allow\": true }, \"_roles\": { \"viewer\": { \"allow\": [\"get\"] } } }");
    chk("_default {allow:true} (object)",    pgf_policy_allows("other", PGF_ACT_GET, "viewer"), true);

    pgf_policy_cleanup();
    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
