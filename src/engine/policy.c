#include "policy.h"
#include "core/session.h"
#include "logger.h"

#include <cjson/cJSON.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The policy config (JSON), bound per request. g_default is the process default
 * (boot CEL_POLICY_FILE / single-app); t_active is the current request's app
 * policy (set by cel_apps_enter). active() returns the per-thread binding if set,
 * else the default; NULL => built-in role defaults. */
static cJSON          *g_default = NULL;
static __thread cJSON *t_active  = NULL;
static cJSON *active(void) { return t_active ? t_active : g_default; }

/* cel_identity_from_token lives in api.c (it needs cel_auth_resolve) so this
 * module stays a pure policy-config evaluator with no auth dependency. */

static const char *action_name(cel_action_t a) {
    switch (a) {
        case CEL_ACT_LIST:   return "list";
        case CEL_ACT_GET:    return "get";
        case CEL_ACT_CREATE: return "create";
        case CEL_ACT_UPDATE: return "update";
        case CEL_ACT_DELETE: return "delete";
        default:             return "";
    }
}

/* The declarative role bundle. A deployment may define its own role vocabulary
 * (rider/driver, customer/staff, …) under a reserved top-level "_roles" object:
 *
 *   "_roles": {
 *     "admin":  { "superuser": true },
 *     "rider":  { "allow": ["list","get","create"] },
 *     "driver": { "allow": ["list","get","update"] }
 *   }
 *
 * Semantics are *additive/override*, never replacing: an entry is authoritative
 * for that role (its "superuser" flag and "allow" default-action set), while any
 * role NOT mentioned keeps its built-in default (admin = superuser, editor = all
 * but delete, viewer = read, everything else nothing). So adding custom roles
 * never silently strips the built-ins, and the built-ins can still be overridden
 * (e.g. editor → { "allow": [] }, or admin → { "superuser": false }).
 * Returns the role's "_roles" object, or NULL if there is none. */
static const cJSON *role_def(const char *role) {
    if (!active()) return NULL;
    const cJSON *roles = cJSON_GetObjectItemCaseSensitive(active(), "_roles");
    if (!cJSON_IsObject(roles)) return NULL;
    return cJSON_GetObjectItemCaseSensitive(roles, role);
}

/* Is the action name `act` present in the JSON string array `arr`? */
static bool action_in_array(const cJSON *arr, const char *act) {
    const cJSON *e;
    cJSON_ArrayForEach(e, arr)
        if (cJSON_IsString(e) && !strcmp(e->valuestring, act)) return true;
    return false;
}

/* Default action set for a role on a table that has no explicit policy entry.
 * Declarative when the role has an "_roles" entry (its "allow" list; nothing if
 * absent); otherwise the built-in defaults. Superusers never reach here — they
 * short-circuit in cel_policy_allows. */
static bool default_allows(cel_action_t a, const char *role) {
    const cJSON *rd = role_def(role);
    if (rd) {
        const cJSON *allow = cJSON_GetObjectItemCaseSensitive(rd, "allow");
        return cJSON_IsArray(allow) && action_in_array(allow, action_name(a));
    }
    if (!strcmp(role, "editor")) return a != CEL_ACT_DELETE;
    if (!strcmp(role, "viewer")) return a == CEL_ACT_LIST || a == CEL_ACT_GET;
    return false;   /* anon / unknown roles: nothing */
}

/* Roles that may perform any action and bypass owner-column row scoping.
 * Declarative: a role is a policy-layer superuser iff its "_roles" entry sets
 * "superuser": true; with no entry the built-in default applies ("admin" is the
 * superuser). "platform_admin" is ALWAYS a superuser here — the global operator,
 * created out-of-band; config may grant superuser to others but can never revoke
 * it from platform_admin. NOTE: tenant *scope* still confines "admin" to its own
 * tenant — only platform_admin skips it (see make_scope in api.c). */
static bool is_superuser_role(const char *role) {
    if (!strcmp(role, "platform_admin")) return true;
    const cJSON *rd = role_def(role);
    if (rd) return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rd, "superuser"));
    return !strcmp(role, "admin");   /* built-in default */
}

/* Is `role` listed in the JSON array `roles`? */
static bool role_in_array(const cJSON *roles, const char *role) {
    const cJSON *e;
    cJSON_ArrayForEach(e, roles)
        if (cJSON_IsString(e) && !strcmp(e->valuestring, role)) return true;
    return false;
}

/* Fail-closed default for a table with no explicit policy entry. When a config is
 * loaded, an unlisted table is DENIED unless the operator opts back into the
 * permissive built-in/_roles fallback via a top-level "_default". Both spellings
 * are accepted, so the documented object form and the terse string form both work:
 *   "_default": "allow" | "deny"
 *   "_default": { "allow": true } | { "deny": true }   (deny:false == allow)
 * Absent or unrecognized => deny (cel_policy_init warns about this at load).
 *
 * SECURITY (H-1): previously an unlisted table fell through to the built-in
 * editor/viewer grants unless "_default" was EXACTLY the string "deny" — so a
 * loaded policy that locked down some tables left every other table fully
 * accessible, and the documented object form ({"deny":true}) was silently ignored. */
static bool policy_default_is_allow(void) {
    const cJSON *def = cJSON_GetObjectItemCaseSensitive(active(), "_default");
    if (cJSON_IsString(def)) return !strcmp(def->valuestring, "allow");
    if (cJSON_IsObject(def)) {
        const cJSON *deny  = cJSON_GetObjectItemCaseSensitive(def, "deny");
        const cJSON *allow = cJSON_GetObjectItemCaseSensitive(def, "allow");
        if (cJSON_IsBool(deny))  return !cJSON_IsTrue(deny);   /* deny:false => allow */
        if (cJSON_IsBool(allow)) return cJSON_IsTrue(allow);
    }
    return false;   /* fail closed */
}

/* True if the table entry explicitly lists at least one CRUD action
 * (list/get/create/update/delete). A meta-only entry — e.g. just
 * {"realtime": true} — lists none, so it must NOT fail-close CRUD: it should
 * behave like an unlisted table and fall through to "_default". Only once an
 * entry lists SOME action is it an explicit allow-list whose unlisted actions
 * deny (the H-1 guarantee). This removes the footgun where adding "realtime":
 * true to opt a table into change events silently denied all of its CRUD. */
static bool table_lists_any_action(const cJSON *tbl) {
    static const cel_action_t acts[] = {
        CEL_ACT_LIST, CEL_ACT_GET, CEL_ACT_CREATE, CEL_ACT_UPDATE, CEL_ACT_DELETE };
    for (size_t i = 0; i < sizeof acts / sizeof acts[0]; i++)
        if (cJSON_GetObjectItemCaseSensitive(tbl, action_name(acts[i]))) return true;
    return false;
}

bool cel_policy_allows(const char *table, cel_action_t action, const char *role) {
    if (is_superuser_role(role)) return true;

    if (active()) {
        const cJSON *tbl = cJSON_GetObjectItemCaseSensitive(active(), table);
        if (!tbl) {
            /* No explicit entry: fail closed. The permissive built-in/_roles
             * fallback applies only if the operator set "_default":"allow". */
            return policy_default_is_allow() ? default_allows(action, role) : false;
        }
        /* an action may be a role array, or an object {roles, owner_column} */
        const cJSON *act = cJSON_GetObjectItemCaseSensitive(tbl, action_name(action));
        const cJSON *roles = cJSON_IsArray(act) ? act
                           : (cJSON_IsObject(act) ? cJSON_GetObjectItemCaseSensitive(act, "roles")
                                                  : NULL);
        if (!cJSON_IsArray(roles)) {
            /* This action isn't listed. If the entry lists NO action at all it's
             * metadata-only (e.g. {"realtime": true}) — treat it like an unlisted
             * table and consult "_default" rather than silently denying every CRUD
             * action. If it lists some actions, it's an explicit allow-list and the
             * unlisted action stays denied (H-1). */
            if (table_lists_any_action(tbl)) return false;
            return policy_default_is_allow() ? default_allows(action, role) : false;
        }
        return role_in_array(roles, role);
    }
    return default_allows(action, role);
}

const char *cel_policy_owner_column(const char *table, cel_action_t action, const char *role) {
    if (!active()) return NULL;                 /* built-in defaults: no row scoping */
    if (is_superuser_role(role)) return NULL;   /* superuser sees/edits all rows */

    const cJSON *tbl = cJSON_GetObjectItemCaseSensitive(active(), table);
    if (!tbl) return NULL;
    const cJSON *act = cJSON_GetObjectItemCaseSensitive(tbl, action_name(action));
    if (!cJSON_IsObject(act)) return NULL;
    const cJSON *oc = cJSON_GetObjectItemCaseSensitive(act, "owner_column");
    return cJSON_IsString(oc) ? oc->valuestring : NULL;
}

bool cel_role_is_superuser(const char *role) {
    return is_superuser_role(role);
}

bool cel_policy_realtime_enabled(const char *table) {
    if (!active()) return false;
    const cJSON *tbl = cJSON_GetObjectItemCaseSensitive(active(), table);
    if (!cJSON_IsObject(tbl)) return false;
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(tbl, "realtime"));
}

bool cel_policy_rpc_allows(const char *fn, const char *role) {
    if (!active()) return false;
    const cJSON *rpc = cJSON_GetObjectItemCaseSensitive(active(), "_rpc");
    if (!cJSON_IsObject(rpc)) return false;
    const cJSON *entry = cJSON_GetObjectItemCaseSensitive(rpc, fn);
    if (!cJSON_IsObject(entry)) return false;          /* not whitelisted => deny */
    if (is_superuser_role(role)) return true;          /* whitelisted: superuser bypasses roles */
    return role_in_array(cJSON_GetObjectItemCaseSensitive(entry, "roles"), role);
}

int cel_policy_rpc_names(const char **out, int max) {
    if (!active() || !out || max <= 0) return 0;
    const cJSON *rpc = cJSON_GetObjectItemCaseSensitive(active(), "_rpc");
    if (!cJSON_IsObject(rpc)) return 0;
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, rpc) {
        if (n >= max) break;
        if (e->string) out[n++] = e->string;
    }
    return n;
}

bool cel_role_can_self_register(const char *role) {
    if (!role || !*role) return false;
    if (is_superuser_role(role)) return false;   /* boundary: never via signup */
    const cJSON *rd = role_def(role);
    if (!rd) return false;                        /* deny by default */
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rd, "self_register"));
}

bool cel_role_default_signup(char *out, size_t out_len) {
    if (!active()) return false;
    const cJSON *roles = cJSON_GetObjectItemCaseSensitive(active(), "_roles");
    if (!cJSON_IsObject(roles)) return false;
    const cJSON *e;
    cJSON_ArrayForEach(e, roles) {
        if (e->string && cel_role_can_self_register(e->string)) {
            snprintf(out, out_len, "%s", e->string);
            return true;
        }
    }
    return false;
}

bool cel_policy_owner_scope(const char *table, cel_action_t action,
                            const char *role, cel_owner_spec_t *out) {
    memset(out, 0, sizeof *out);
    if (!active()) return false;
    if (is_superuser_role(role)) return false;   /* superuser: never row-scoped */

    const cJSON *tbl = cJSON_GetObjectItemCaseSensitive(active(), table);
    if (!tbl) return false;
    const cJSON *act = cJSON_GetObjectItemCaseSensitive(tbl, action_name(action));
    if (!cJSON_IsObject(act)) return false;

    /* EQ — owner_column (back-compat, the common case) */
    const cJSON *oc = cJSON_GetObjectItemCaseSensitive(act, "owner_column");
    if (cJSON_IsString(oc)) {
        out->kind = CEL_OWNER_EQ; out->column = oc->valuestring;
        return true;
    }
    /* OR — owner_any: [columns] */
    const cJSON *oa = cJSON_GetObjectItemCaseSensitive(act, "owner_any");
    if (cJSON_IsArray(oa)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, oa) {
            if (out->ncolumns >= CEL_MAX_OWNER_COLS) break;
            if (cJSON_IsString(e)) out->columns[out->ncolumns++] = e->valuestring;
        }
        if (out->ncolumns > 0) { out->kind = CEL_OWNER_ANY; return true; }
    }
    /* VIA — owner_via: {table, ref, local, user} */
    const cJSON *ov = cJSON_GetObjectItemCaseSensitive(act, "owner_via");
    if (cJSON_IsObject(ov)) {
        const cJSON *tb = cJSON_GetObjectItemCaseSensitive(ov, "table");
        const cJSON *rf = cJSON_GetObjectItemCaseSensitive(ov, "ref");
        const cJSON *lo = cJSON_GetObjectItemCaseSensitive(ov, "local");
        const cJSON *us = cJSON_GetObjectItemCaseSensitive(ov, "user");
        if (cJSON_IsString(tb) && cJSON_IsString(rf) && cJSON_IsString(lo) && cJSON_IsString(us)) {
            out->kind = CEL_OWNER_VIA;
            out->via.table = tb->valuestring; out->via.ref   = rf->valuestring;
            out->via.local = lo->valuestring; out->via.user  = us->valuestring;
            return true;
        }
    }
    return false;
}

/* Read + parse a policy JSON file. Returns the parsed object, or NULL (no path /
 * unreadable / empty / parse error); sets *parse_error when a file was present but
 * failed to parse. Warns when a loaded config omits "_default" (fail-closed). */
static cJSON *load_policy_json(const char *path, int *parse_error) {
    if (parse_error) *parse_error = 0;
    if (!path || !*path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    cJSON *c = cJSON_Parse(buf);
    free(buf);
    if (!c) { LOG_ERROR("policy: failed to parse '%s'", path); if (parse_error) *parse_error = 1; return NULL; }
    /* Fail closed: warn if no default is declared, since unlisted tables then DENY
     * rather than fall through to the built-in grants. */
    if (!cJSON_HasObjectItem(c, "_default"))
        LOG_WARN("policy: no \"_default\" in '%s' — tables without an explicit entry are "
                 "DENIED. Set \"_default\":\"allow\" to keep the permissive built-in/_roles "
                 "fallback for unlisted tables.", path);
    return c;
}

int cel_policy_init(const char *config_path) {
    if (!config_path || !*config_path) {
        LOG_INFO("policy: using built-in role defaults (no config file)");
        return 0;
    }
    int perr = 0;
    g_default = load_policy_json(config_path, &perr);
    if (g_default)      LOG_INFO("policy: loaded default overrides from '%s'", config_path);
    else if (!perr)     LOG_WARN("policy: config '%s' not found/empty — using defaults", config_path);
    return perr ? -1 : 0;
}

/* Load a per-app policies.json into its own object (or NULL when absent/empty/
 * unparseable → the app falls back to the process default / built-ins). */
cel_policy_t *cel_policy_load(const char *config_path) {
    cJSON *c = load_policy_json(config_path, NULL);
    if (c) LOG_INFO("policy: loaded app overrides from '%s'", config_path);
    return (cel_policy_t *)c;
}
void cel_policy_free(cel_policy_t *p)       { cJSON_Delete((cJSON *)p); }
void cel_policy_set_active(cel_policy_t *p) { t_active = (cJSON *)p; }
void cel_policy_clear_active(void)          { t_active = NULL; }

/* Resolve the `_session` block of the active policy into a session policy. Starts
 * from the built-in default (fixed, 24h) and overlays whatever the block sets; an
 * unknown strategy is logged and left at the safe default. Numbers <= 0 (or < 0 for
 * the cap) are ignored, keeping the default. */
void cel_policy_session(cel_session_policy_t *out) {
    if (!out) return;
    cel_session_policy_default(out);
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(active(), "_session");
    if (!cJSON_IsObject(s)) return;

    bool ttl_set = false;
    const cJSON *strat = cJSON_GetObjectItemCaseSensitive(s, "strategy");
    if (cJSON_IsString(strat) && strat->valuestring) {
        if      (strcmp(strat->valuestring, "fixed")   == 0) out->strategy = CEL_SESSION_FIXED;
        else if (strcmp(strat->valuestring, "sliding") == 0) out->strategy = CEL_SESSION_SLIDING;
        else LOG_ERROR("policy: unknown _session.strategy '%s' — using the safe default (fixed)",
                       strat->valuestring);
    }
    /* Clamp to INT_MAX before narrowing: (int)valuedouble is UB once the double
     * exceeds INT_MAX (a JSON number can), and the overflow would land negative —
     * a past expires_at (instantly-expired sessions) or a disabled cap. */
    const cJSON *ttl = cJSON_GetObjectItemCaseSensitive(s, "ttl_seconds");
    if (cJSON_IsNumber(ttl) && ttl->valuedouble > 0) {
        out->ttl_seconds = ttl->valuedouble >= (double)INT_MAX ? INT_MAX : (int)ttl->valuedouble;
        ttl_set = true;
    }
    const cJSON *cap = cJSON_GetObjectItemCaseSensitive(s, "absolute_max_seconds");
    if (cJSON_IsNumber(cap) && cap->valuedouble >= 0)
        out->absolute_max_seconds = cap->valuedouble >= (double)INT_MAX ? INT_MAX : (int)cap->valuedouble;

    /* Device tokens are opt-in: a positive lifetime enables the feature; absent/<=0 keeps it off. */
    const cJSON *dev = cJSON_GetObjectItemCaseSensitive(s, "device_ttl_seconds");
    if (cJSON_IsNumber(dev) && dev->valuedouble > 0)
        out->device_ttl_seconds = dev->valuedouble >= (double)INT_MAX ? INT_MAX : (int)dev->valuedouble;

    /* sliding without an explicit ttl: the idle window defaults to 1h, not fixed's 24h. */
    if (out->strategy == CEL_SESSION_SLIDING && !ttl_set) out->ttl_seconds = 3600;
}

void cel_policy_cleanup(void) {
    if (g_default) { cJSON_Delete(g_default); g_default = NULL; }
}
