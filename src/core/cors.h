/* ============================================================================
 * cellar — CORS policy (origin allowlist for browser frontends).
 *
 * A browser SPA on a different origin (storefront / dashboard) can't call the API
 * unless the server returns CORS headers. This module holds the policy; the HTTP
 * router (http_routes.c) emits the headers and answers OPTIONS preflights. OPT-IN
 * and locked down by default: with no CEL_CORS_ORIGINS set, no CORS headers are
 * sent (same-origin only, the current behavior).
 * ============================================================================ */
#ifndef CEL_CORS_H
#define CEL_CORS_H

#include <stdbool.h>

/* Read config from the environment (call once at startup):
 *   CEL_CORS_ORIGINS      comma list of allowed origins, or "*" for any. Unset/empty
 *                         = CORS disabled.
 *   CEL_CORS_CREDENTIALS  "1" to send Access-Control-Allow-Credentials: true. With
 *                         credentials, "*" is not allowed — the specific origin is
 *                         echoed instead (per the CORS spec). Default off. */
void cel_cors_init(void);

/* True if any origin is configured. */
bool cel_cors_enabled(void);

/* The Access-Control-Allow-Origin value to send for a request whose Origin header
 * is `origin` (NUL-terminated), or NULL if the origin is not allowed (=> no CORS
 * headers). Returns either the static literal "*" (wildcard; credentials are
 * forced off in that mode, audit 2026-08 #3) or a stable pointer into the
 * configured allowlist — never the caller's `origin` buffer, so the result does
 * not depend on `origin` staying alive. */
const char *cel_cors_allow_origin(const char *origin);

/* True if Access-Control-Allow-Credentials: true should be sent. */
bool cel_cors_allow_credentials(void);

#endif /* CEL_CORS_H */
