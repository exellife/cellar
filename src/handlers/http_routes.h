/* cellar — REST front door: maps HTTP requests onto the engine API. */
#ifndef CEL_HTTP_ROUTES_H
#define CEL_HTTP_ROUTES_H

#include "portico.h"
#include "core/rate_limit.h"

/* portico on_http_request handler. Routes:
 *   GET  /schema              -> schema catalog
 *   GET  /api/<table>         -> list (querystring: select,order,limit,offset,<col>=<op>.<val>)
 *   GET  /api/<table>/<id>    -> get by primary key
 *   POST /auth/login          -> login (JSON body)
 */
int cel_http_router(const portico_request_t *req, portico_response_t *res, void *user_data);

/* Cap the accepted request-body size (bytes); larger bodies get 413 before they
 * are parsed. 0 disables the cap. Set once at startup from CEL_MAX_BODY. */
void cel_http_set_max_body(size_t max_bytes);

/* Install the rate limiters (owned by main.c; either may be NULL = that throttle
 * off): `auth_rl` guards the auth endpoints, `api_rl` throttles the data API + RPC. */
void cel_http_set_rate_limiters(cel_ratelimit_t *auth_rl, cel_ratelimit_t *api_rl);

#endif /* CEL_HTTP_ROUTES_H */
