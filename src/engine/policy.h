/* ============================================================================
 * cellar — identity & authorization policy.
 *
 * Deny-by-default. Every data operation resolves a caller identity from a session
 * token, then checks a per-table, per-action policy before touching the DB. The
 * built-in defaults (admin = all, editor = read+create+update, viewer = read,
 * anon = nothing) apply unless a policies.json overrides a table. `admin` is a
 * superuser (always allowed).
 *
 * Roles are not baked into the engine: a deployment defines its own vocabulary
 * (rider/driver, customer/staff, …) under a reserved "_roles" object in the
 * policy config — each entry sets "superuser" and/or a default-action "allow"
 * list. _roles entries override or extend the built-ins additively (a role not
 * mentioned keeps its built-in default; `platform_admin` is always a superuser).
 * The engine treats every role as an opaque string, so "configure, don't fork".
 * ============================================================================ */
#ifndef CEL_POLICY_H
#define CEL_POLICY_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool authenticated;
    char user_id[37];
    char role[32];      /* admin | editor | viewer | platform_admin | "anon" when unauthenticated */
    char tenant_id[37]; /* the caller's tenant UUID; "" in single-tenant deployments
                         * (and for platform_admin, who is global). Read by the scope
                         * engine to apply tenant scoping in pooled mode. */
} cel_identity_t;

typedef enum {
    CEL_ACT_LIST,
    CEL_ACT_GET,
    CEL_ACT_CREATE,
    CEL_ACT_UPDATE,
    CEL_ACT_DELETE,
} cel_action_t;

/* Resolve a session token (may be NULL/empty) to an identity. Unknown/expired
 * tokens yield an unauthenticated identity with role "anon". */
void cel_identity_from_token(const char *token, cel_identity_t *out);

/* True if `role` may perform `action` on `table`. */
bool cel_policy_allows(const char *table, cel_action_t action, const char *role);

/* If (table, action) is row-scoped to the caller, returns the ownership column
 * name (the row's owner must equal the caller's user id); otherwise NULL.
 * admin is a superuser and is never row-scoped. (EQ scoping only — see
 * cel_policy_owner_scope for the OR / relationship kinds.) */
const char *cel_policy_owner_column(const char *table, cel_action_t action, const char *role);

/* The richer owner-scoping spec for (table, action, role): a single column (EQ),
 * any of several columns (OR), or membership in a related table (VIA). Configured
 * in policies.json per action as one of:
 *   "owner_column": "c"
 *   "owner_any":    ["c1","c2", ...]
 *   "owner_via":    { "table": "...", "ref": "...", "local": "...", "user": "..." }
 * Returns true and fills *out when there is owner scoping; false (kind NONE) for
 * superusers, unscoped actions, or no config. Strings point into the loaded
 * config and are valid until cel_policy_cleanup. */
typedef enum { CEL_OWNER_NONE = 0, CEL_OWNER_EQ, CEL_OWNER_ANY, CEL_OWNER_VIA } cel_owner_kind_t;
#define CEL_MAX_OWNER_COLS 4
typedef struct {
    cel_owner_kind_t kind;
    const char *column;                        /* EQ */
    const char *columns[CEL_MAX_OWNER_COLS];   /* ANY */
    int ncolumns;
    struct { const char *table, *ref, *local, *user; } via;  /* VIA */
} cel_owner_spec_t;

bool cel_policy_owner_scope(const char *table, cel_action_t action,
                            const char *role, cel_owner_spec_t *out);

/* Load an optional policy config file (JSON). Pass NULL to use built-in defaults.
 * Returns 0 on success (or when no file is configured), -1 on parse error. */
int cel_policy_init(const char *config_path);
void cel_policy_cleanup(void);

/* True if `role` is a policy-layer superuser (per the _roles config / built-ins:
 * admin and platform_admin by default). Superusers bypass role + row checks. */
bool cel_role_is_superuser(const char *role);

/* True if `table` is opted into realtime change events ("realtime": true in its
 * policy-config entry). Deny-by-default: no config / no flag ⇒ false. */
bool cel_policy_realtime_enabled(const char *table);

/* True if `role` may call the RPC function `fn`. Deny-by-default: the function
 * must be whitelisted under "_rpc" (so even a superuser cannot reach a function
 * that is not exposed), then a superuser bypasses the per-function roles list:
 *   "_rpc": { "request_ride": { "roles": ["rider","admin"] } } */
bool cel_policy_rpc_allows(const char *fn, const char *role);

/* Fill `out` with up to `max` whitelisted RPC function names (pointers into the
 * loaded config, valid for the process lifetime); returns the count. Used at
 * startup to audit which exposed functions are SECURITY DEFINER (H-4). */
int cel_policy_rpc_names(const char **out, int max);

/* ---- self-service registration (Phase 8) ---------------------------------- */

/* True if `role` may be obtained via self-service signup: it must be flagged
 * "self_register": true in the _roles config AND not be a superuser. Privileged
 * roles are provisioned out-of-band only — the boundary that stops signup from
 * escalating. Deny-by-default: unknown/unflagged roles return false. */
bool cel_role_can_self_register(const char *role);

/* Writes the default signup role (the first self-registerable role in _roles)
 * into `out` and returns true; returns false if no role is self-registerable.
 * Used when a register request omits an explicit role. */
bool cel_role_default_signup(char *out, size_t out_len);

#endif /* CEL_POLICY_H */
