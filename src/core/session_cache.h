/* ============================================================================
 * cellar — in-memory session cache.
 *
 * Token verification (cel_auth_verify) runs on EVERY authenticated request — a
 * cel_sessions⋈cel_users join. Under load that DB round-trip is the dominant auth
 * cost. This cache maps a token → its resolved user for a bounded TTL, so most
 * requests skip the DB. It is OPT-IN (CEL_SESSION_CACHE_TTL>0); with ttl<=0 it is
 * inert and every request hits the DB exactly as before.
 *
 * Coherence: logout evicts immediately, so same-instance logout stays instant.
 * Out-of-band changes (a suspended tenant, a changed role, a session expiring)
 * are re-checked against the DB at most TTL seconds later — the staleness window
 * is the whole knob. Lower TTL = fresher; higher TTL = fewer DB hits.
 * (Multi-instance: another instance's logout is not seen until TTL — same class
 * of caveat as #53b; pair with a short TTL where that matters.)
 * ============================================================================ */
#ifndef CEL_SESSION_CACHE_H
#define CEL_SESSION_CACHE_H

#include "auth.h"        /* cel_user_t */
#include <stdbool.h>

/* Set the cache TTL in seconds; <=0 disables the cache (inert get/put). */
void cel_session_cache_init(int ttl_seconds);
void cel_session_cache_cleanup(void);

/* On a fresh, non-stale hit fills *user and returns true; otherwise false. */
bool cel_session_cache_get(const char *token, cel_user_t *user);

/* Insert/refresh token → user with a fresh TTL window (no-op when disabled). */
void cel_session_cache_put(const char *token, const cel_user_t *user);

/* Drop a token now (called on logout so revocation is immediate). */
void cel_session_cache_evict(const char *token);

/* Drop ALL cached tokens (the cache can't evict by user id, so a bulk session
 * revocation clears everything; the cache stays enabled and refills on demand). */
void cel_session_cache_clear(void);

#endif /* CEL_SESSION_CACHE_H */
