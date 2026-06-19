/* ============================================================================
 * pgforge — token-bucket rate limiter, keyed by a string (client IP or user id).
 *
 * Each limiter is an INDEPENDENT instance, so a deployment can run a strict bucket
 * on the expensive auth endpoints (Argon2id is brute-forceable + a CPU-DoS
 * amplifier) AND a separate, more generous bucket on the data API. Per-key token
 * bucket: a burst of `limit` is allowed, refilling at limit/window per second;
 * over the limit -> deny (the caller replies 429).
 *
 * Mechanism (the key — client IP from portico, or the authenticated user id) is
 * supplied by the caller; the policy (which endpoints, what limit) lives in the
 * router and main.c.
 * ============================================================================ */
#ifndef PGF_RATE_LIMIT_H
#define PGF_RATE_LIMIT_H

#include <stdbool.h>

typedef struct pgf_ratelimit pgf_ratelimit_t;

/* Create a limiter of `limit` requests per `window_seconds`. Returns NULL when
 * limit <= 0 / window <= 0 (disabled) — a NULL limiter is valid and allows always.
 * Also returns NULL on allocation failure (fail open). */
pgf_ratelimit_t *pgf_ratelimit_create(int limit, int window_seconds);
void pgf_ratelimit_destroy(pgf_ratelimit_t *rl);

/* Consume one token for `key`. Returns true if allowed, false if over the limit.
 * A NULL limiter (disabled), or a NULL/empty key, always returns true. */
bool pgf_ratelimit_allow(pgf_ratelimit_t *rl, const char *key);

#endif /* PGF_RATE_LIMIT_H */
