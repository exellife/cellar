/* ============================================================================
 * pgforge — in-memory session cache.
 *
 * Token verification (pgf_auth_verify) runs on EVERY authenticated request — a
 * pgf_sessions⋈pgf_users join. Under load that DB round-trip is the dominant auth
 * cost. This cache maps a token → its resolved user for a bounded TTL, so most
 * requests skip the DB. It is OPT-IN (PGF_SESSION_CACHE_TTL>0); with ttl<=0 it is
 * inert and every request hits the DB exactly as before.
 *
 * Coherence: logout evicts immediately, so same-instance logout stays instant.
 * Out-of-band changes (a suspended tenant, a changed role, a session expiring)
 * are re-checked against the DB at most TTL seconds later — the staleness window
 * is the whole knob. Lower TTL = fresher; higher TTL = fewer DB hits.
 * (Multi-instance: another instance's logout is not seen until TTL — same class
 * of caveat as #53b; pair with a short TTL where that matters.)
 * ============================================================================ */
#ifndef PGF_SESSION_CACHE_H
#define PGF_SESSION_CACHE_H

#include "auth.h"        /* pgf_user_t */
#include <stdbool.h>

/* Set the cache TTL in seconds; <=0 disables the cache (inert get/put). */
void pgf_session_cache_init(int ttl_seconds);
void pgf_session_cache_cleanup(void);

/* On a fresh, non-stale hit fills *user and returns true; otherwise false. */
bool pgf_session_cache_get(const char *token, pgf_user_t *user);

/* Insert/refresh token → user with a fresh TTL window (no-op when disabled). */
void pgf_session_cache_put(const char *token, const pgf_user_t *user);

/* Drop a token now (called on logout so revocation is immediate). */
void pgf_session_cache_evict(const char *token);

/* Drop ALL cached tokens (the cache can't evict by user id, so a bulk session
 * revocation clears everything; the cache stays enabled and refills on demand). */
void pgf_session_cache_clear(void);

#endif /* PGF_SESSION_CACHE_H */
