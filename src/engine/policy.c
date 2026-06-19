#include "policy.h"
#include "core/auth.h"
#include "logger.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Optional per-table override config; NULL => built-in defaults only. */
static cJSON *g_config = NULL;

/* ---- tenancy configuration ------------------------------------------------ */

static char g_tenant_column[64] = {0};

void cel_tenancy_init(void) {
    const char *c = getenv("CEL_TENANT_COLUMN");
    if (c && *c) snprintf(g_tenant_column, sizeof g_tenant_column, "%s", c);
}

const char *cel_tenancy_column(void) {
    return g_tenant_column[0] ? g_tenant_column : NULL;
}

void cel_identity_from_token(const char *token, cel_identity_t *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->role, sizeof out->role, "%s", "anon");
    if (!token || !*token) return;

    cel_user_t u;
    if (cel_auth_resolve(token, &u) == CEL_AUTH_OK) {   /* cache-aware (no DB hit on a hit) */
        out->authenticated = true;
        snprintf(out->user_id, sizeof out->user_id, "%s", u.id);
        snprintf(out->role, sizeof out->role, "%s", u.role);
        snprintf(out->tenant_id, sizeof out->tenant_id, "%s", u.tenant_id);
    }
}

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
    if (!g_config) return NULL;
    const cJSON *roles = cJSON_GetObjectItemCaseSensitive(g_config, "_roles");
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
    const cJSON *def = cJSON_GetObjectItemCaseSensitive(g_config, "_default");
    if (cJSON_IsString(def)) return !strcmp(def->valuestring, "allow");
    if (cJSON_IsObject(def)) {
        const cJSON *deny  = cJSON_GetObjectItemCaseSensitive(def, "deny");
        const cJSON *allow = cJSON_GetObjectItemCaseSensitive(def, "allow");
        if (cJSON_IsBool(deny))  return !cJSON_IsTrue(deny);   /* deny:false => allow */
        if (cJSON_IsBool(allow)) return cJSON_IsTrue(allow);
    }
    return false;   /* fail closed */
}

bool cel_policy_allows(const char *table, cel_action_t action, const char *role) {
    if (is_superuser_role(role)) return true;

    if (g_config) {
        const cJSON *tbl = cJSON_GetObjectItemCaseSensitive(g_config, table);
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
        if (!cJSON_IsArray(roles)) return false;    /* action not listed => deny */
        return role_in_array(roles, role);
    }
    return default_allows(action, role);
}

const char *cel_policy_owner_column(const char *table, cel_action_t action, const char *role) {
    if (!g_config) return NULL;                 /* built-in defaults: no row scoping */
    if (is_superuser_role(role)) return NULL;   /* superuser sees/edits all rows */

    const cJSON *tbl = cJSON_GetObjectItemCaseSensitive(g_config, table);
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
    if (!g_config) return false;
    const cJSON *tbl = cJSON_GetObjectItemCaseSensitive(g_config, table);
    if (!cJSON_IsObject(tbl)) return false;
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(tbl, "realtime"));
}

bool cel_policy_rpc_allows(const char *fn, const char *role) {
    if (!g_config) return false;
    const cJSON *rpc = cJSON_GetObjectItemCaseSensitive(g_config, "_rpc");
    if (!cJSON_IsObject(rpc)) return false;
    const cJSON *entry = cJSON_GetObjectItemCaseSensitive(rpc, fn);
    if (!cJSON_IsObject(entry)) return false;          /* not whitelisted => deny */
    if (is_superuser_role(role)) return true;          /* whitelisted: superuser bypasses roles */
    return role_in_array(cJSON_GetObjectItemCaseSensitive(entry, "roles"), role);
}

int cel_policy_rpc_names(const char **out, int max) {
    if (!g_config || !out || max <= 0) return 0;
    const cJSON *rpc = cJSON_GetObjectItemCaseSensitive(g_config, "_rpc");
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
    if (!g_config) return false;
    const cJSON *roles = cJSON_GetObjectItemCaseSensitive(g_config, "_roles");
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
    if (!g_config) return false;
    if (is_superuser_role(role)) return false;   /* superuser: never row-scoped */

    const cJSON *tbl = cJSON_GetObjectItemCaseSensitive(g_config, table);
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

int cel_policy_init(const char *config_path) {
    if (!config_path || !*config_path) {
        LOG_INFO("policy: using built-in role defaults (no config file)");
        return 0;
    }
    FILE *f = fopen(config_path, "rb");
    if (!f) { LOG_WARN("policy: config '%s' not found — using defaults", config_path); return 0; }

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';

    g_config = cJSON_Parse(buf);
    free(buf);
    if (!g_config) { LOG_ERROR("policy: failed to parse '%s'", config_path); return -1; }
    LOG_INFO("policy: loaded overrides from '%s'", config_path);
    /* Fail closed: warn loudly if the operator didn't declare a default, since
     * unlisted tables now DENY rather than fall through to the built-in grants. */
    if (!cJSON_HasObjectItem(g_config, "_default"))
        LOG_WARN("policy: no \"_default\" in '%s' — tables without an explicit entry are "
                 "DENIED. Set \"_default\":\"allow\" to keep the permissive built-in/_roles "
                 "fallback for unlisted tables.", config_path);
    return 0;
}

void cel_policy_cleanup(void) {
    if (g_config) { cJSON_Delete(g_config); g_config = NULL; }
}
