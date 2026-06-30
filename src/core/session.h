#ifndef CEL_SESSION_H
#define CEL_SESSION_H

/* Per-app session management behind a strategy seam. The engine resolves a
 * bundle's `_session` policy (policies.json) and binds it per request via
 * cel_session_set_active; the auth layer (cel_auth_issue_session / cel_auth_verify)
 * delegates to the active strategy. Adapters are C — the verify path is the hot,
 * security-critical path. See docs/session-management.md (incl. the future
 * code-pluggable strategies: JWT/stateless, external/multi-instance store). */

#include <stddef.h>
#include "auth.h"   /* cel_user_t, CEL_AUTH_* */

typedef enum {
    CEL_SESSION_FIXED   = 0,   /* absolute lifetime, no renewal (the default = today) */
    CEL_SESSION_SLIDING = 1,   /* idle window, renewed on use, optional absolute cap */
} cel_session_strategy_e;

/* Resolved per-app session policy — plain data, filled by the engine from the
 * `_session` block (or the built-in default). Core never parses JSON. */
typedef struct {
    cel_session_strategy_e strategy;
    int ttl_seconds;           /* fixed: absolute lifetime; sliding: the idle window */
    int absolute_max_seconds;  /* sliding only: hard cap from created_at; 0 = none */
    int device_ttl_seconds;    /* device-token lifetime; 0 = device tokens DISABLED for the app */
} cel_session_policy_t;

/* The built-in default an app inherits when it has no `_session` block: the
 * historical behavior (fixed, 24h). */
void cel_session_policy_default(cel_session_policy_t *out);

/* The strategy seam. revoke is strategy-independent (delete the session row) so it
 * stays in auth.c (cel_auth_logout). A future stateless/JWT or external-store
 * adapter implements these against its own backing — callers depend on this
 * interface, never on cel_sessions directly. */
typedef struct {
    /* mint a session for an authenticated user; writes the opaque token (>= 65). */
    int (*issue)(const cel_session_policy_t *pol, const char *user_id,
                 char *out_token, size_t token_size);
    /* resolve a token → user, applying expiry/renewal per the policy (may write). */
    int (*verify)(const cel_session_policy_t *pol, const char *token, cel_user_t *out);
} cel_session_strategy_t;

/* Map a strategy enum to its adapter. Never NULL for a known value; NULL for an
 * unrecognized enum (callers fall back to the default strategy). */
const cel_session_strategy_t *cel_session_strategy_for(cel_session_strategy_e s);

/* Per-request active policy (thread-local, mirrors cel_policy_set_active). The
 * engine copies in the resolved policy at app-bind; clears it after. When unset
 * (CLI/seeding paths), cel_session_active() returns the built-in default. */
void cel_session_set_active(const cel_session_policy_t *pol);   /* copies by value */
void cel_session_clear_active(void);
const cel_session_policy_t *cel_session_active(void);

#endif /* CEL_SESSION_H */
