/* Per-app session policy: the thread-local active policy + the built-in default.
 * Dependency-free on purpose (no DB/crypto) so engine TUs that only bind the policy
 * (cel_apps, policy) can link this without pulling sqlite/sodium. The DB-backed
 * strategy adapters live in session_strategy.c. See docs/session-management.md. */
#include "session.h"

#include <stdbool.h>

static __thread cel_session_policy_t t_policy;
static __thread bool                 t_policy_set = false;

void cel_session_policy_default(cel_session_policy_t *out) {
    if (!out) return;
    out->strategy = CEL_SESSION_FIXED;
    out->ttl_seconds = 24 * 3600;       /* the historical default */
    out->absolute_max_seconds = 0;
    out->device_ttl_seconds = 0;        /* device tokens off unless the app opts in */
}

void cel_session_set_active(const cel_session_policy_t *pol) {
    if (!pol) { t_policy_set = false; return; }
    t_policy = *pol;                    /* copy by value — caller's struct is transient */
    t_policy_set = true;
}
void cel_session_clear_active(void) { t_policy_set = false; }

const cel_session_policy_t *cel_session_active(void) {
    static const cel_session_policy_t dflt = { CEL_SESSION_FIXED, 24 * 3600, 0, 0 };
    return t_policy_set ? &t_policy : &dflt;
}
