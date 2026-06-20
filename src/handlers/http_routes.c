#include "http_routes.h"
#include "engine/api.h"
#include "engine/policy.h"
#include "engine/openapi.h"
#include "engine/cel_apps.h"
#include "core/rate_limit.h"
#include "core/metrics.h"
#include "core/cors.h"
#include "web_assets.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sodium.h>
#include <sys/stat.h>

/* Max accepted request body (bytes); 0 = no cap. Set from CEL_MAX_BODY at startup. */
static size_t g_max_body = 0;
void cel_http_set_max_body(size_t max_bytes) { g_max_body = max_bytes; }

/* Rate limiters owned by main.c (NULL = that throttle disabled): g_auth_rl guards
 * the auth endpoints per IP; g_api_rl throttles the data API + RPC per caller. */
static cel_ratelimit_t *g_auth_rl = NULL;
static cel_ratelimit_t *g_api_rl  = NULL;
void cel_http_set_rate_limiters(cel_ratelimit_t *auth_rl, cel_ratelimit_t *api_rl) {
    g_auth_rl = auth_rl; g_api_rl = api_rl;
}

/* ---- small helpers --------------------------------------------------------- */

static int copy_str(char *dst, size_t cap, const char *src, size_t len) {
    if (len >= cap) return -1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* In-place URL-decode (%XX and '+'). */
static void urldecode(char *s) {
    char *o = s;
    for (char *p = s; *p; ) {
        if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            *o++ = (char)((hexval(p[1]) << 4) | hexval(p[2]));
            p += 3;
        } else if (*p == '+') { *o++ = ' '; p++; }
        else { *o++ = *p++; }
    }
    *o = '\0';
}

/* Serialize an engine result into the HTTP response (JSON). Returns the status so
 * the router can classify it for metrics. */
static int send_api(portico_response_t *res, cel_api_result_t ar) {
    portico_res_status(res, ar.http_status);
    char *s = cJSON_PrintUnformatted(ar.body);
    cJSON_Delete(ar.body);
    if (s) { portico_res_body(res, s, strlen(s), "application/json"); free(s); }
    return ar.http_status;
}

/* Consistent JSON error body: {"status":"error","message":...}. Returns status. */
static int send_error(portico_response_t *res, int status, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "error");
    cJSON_AddStringToObject(o, "message", message);
    portico_res_status(res, status);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) { portico_res_body(res, s, strlen(s), "application/json"); free(s); }
    return status;
}

static void add_csv_array(cJSON *parent, const char *key, char *val) {
    cJSON *arr = cJSON_AddArrayToObject(parent, key);
    char *save = NULL;   /* strtok_r: nested tokenizing must not clobber the caller */
    for (char *tok = strtok_r(val, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
        cJSON_AddItemToArray(arr, cJSON_CreateString(tok));
}

static int is_operator(const char *op) {
    static const char *ops[] = {"eq","neq","lt","lte","gt","gte","like","ilike","in","is",0};
    for (int i = 0; ops[i]; i++) if (!strcmp(op, ops[i])) return 1;
    return 0;
}

/* Turn one "key=value" filter into where[col] = { op: value }. PostgREST-ish:
 * value "op.payload" selects the operator; bare value defaults to eq. */
static void add_filter(cJSON *where, const char *key, char *value) {
    const char *op = "eq";
    char *payload = value;
    char *dot = strchr(value, '.');
    if (dot) {
        *dot = '\0';
        if (is_operator(value)) { op = value; payload = dot + 1; }
        else *dot = '.';   /* not an operator prefix; treat whole thing as value */
    }
    cJSON *cond = cJSON_GetObjectItemCaseSensitive(where, key);
    if (!cond) cond = cJSON_AddObjectToObject(where, key);

    if (!strcmp(op, "in")) {
        /* accept both cellar's `in.a,b` and PostgREST's `in.(a,b)` paren form */
        size_t pl = strlen(payload);
        if (pl >= 2 && payload[0] == '(' && payload[pl - 1] == ')') { payload[pl - 1] = '\0'; payload++; }
        cJSON *arr = cJSON_CreateArray();
        char *save = NULL;
        for (char *tok = strtok_r(payload, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
            cJSON_AddItemToArray(arr, cJSON_CreateString(tok));
        cJSON_AddItemToObject(cond, "in", arr);
    } else if (!strcmp(op, "is")) {
        /* PostgREST-style null tests: is.null / is.not.null (the builder maps a JSON
         * null on eq/neq to SQL IS [NOT] NULL). */
        if (!strcmp(payload, "null"))           cJSON_AddNullToObject(cond, "eq");
        else if (!strcmp(payload, "not.null") ||
                 !strcmp(payload, "notnull"))   cJSON_AddNullToObject(cond, "neq");
        /* unknown is.* payload → ignored (no condition added) */
    } else {
        cJSON_AddStringToObject(cond, op, payload);
    }
}

/* Build a list request object from the table name and (decoded) query string. */
static cJSON *build_list_req(const char *table, const char *query, size_t query_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "table", table);
    if (!query || query_len == 0) return req;

    char buf[4096];
    if (copy_str(buf, sizeof buf, query, query_len) != 0) return req;  /* ignore huge qs */

    cJSON *where = NULL;
    char *save = NULL;
    for (char *pair = strtok_r(buf, "&", &save); pair; pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = pair, *val = eq + 1;
        urldecode(key); urldecode(val);
        if      (!strcmp(key, "select")) add_csv_array(req, "select", val);
        else if (!strcmp(key, "order"))  add_csv_array(req, "order", val);
        else if (!strcmp(key, "embed"))  add_csv_array(req, "embed", val);   /* ?embed=categories,reviews */
        else if (!strcmp(key, "group"))  add_csv_array(req, "group", val);   /* ?group=category_id (aggregate) */
        else if (!strcmp(key, "aggregate")) add_csv_array(req, "aggregate", val); /* ?aggregate=count,sum:price */
        else if (!strcmp(key, "count"))  cJSON_AddStringToObject(req, "count", val); /* ?count=exact */
        else if (!strcmp(key, "cursor")) cJSON_AddStringToObject(req, "cursor", val); /* ?cursor=<token> (keyset) */
        else if (!strcmp(key, "limit"))  cJSON_AddNumberToObject(req, "limit", atol(val));
        else if (!strcmp(key, "offset")) cJSON_AddNumberToObject(req, "offset", atol(val));
        else if (!strcmp(key, "where")) {
            /* ?where=<url-encoded JSON tree>: merge its keys (and/or/not/<col>) into
             * the where object, so it combines (AND) with any flat col=op.val filters. */
            cJSON *w = cJSON_Parse(val);
            if (cJSON_IsObject(w)) {
                if (!where) where = cJSON_AddObjectToObject(req, "where");
                cJSON *child;
                while ((child = w->child)) {
                    cJSON_DetachItemViaPointer(w, child);
                    cJSON_AddItemToObject(where, child->string, child);
                }
            }
            cJSON_Delete(w);
        }
        else {
            if (!where) where = cJSON_AddObjectToObject(req, "where");
            add_filter(where, key, val);
        }
    }
    return req;
}

/* ---- router ---------------------------------------------------------------- */

/* Resolve the caller identity from an "Authorization: Bearer <token>" header. */
static void identity_from_request(const portico_request_t *req, cel_identity_t *who) {
    size_t alen = 0;
    const char *auth = portico_req_header(req, "Authorization", &alen);
    char tok[256];
    const char *token = NULL;
    if (auth && alen > 7 && strncasecmp(auth, "Bearer ", 7) == 0) {
        size_t tlen = alen - 7;
        if (tlen < sizeof tok) { memcpy(tok, auth + 7, tlen); tok[tlen] = '\0'; token = tok; }
    }
    cel_identity_from_token(token, who);
}

/* Route a request and return the HTTP status it set, so cel_http_router can record
 * the latency + status-class metrics in one place. */
static int route(const portico_request_t *req, portico_response_t *res) {
    /* GET /health — liveness, public, no DB hit */
    if (portico_req_method_is(req, "GET") && portico_req_path_is(req, "/health")) {
        portico_res_status(res, 200);
        portico_res_body(res, "{\"status\":\"ok\"}", 15, "application/json");
        return 200;
    }

    /* GET /metrics — Prometheus text exposition. NOT open by default (L-3): the
     * scrape leaks the exact build version (CVE targeting) and live operational /
     * auth telemetry. Served only when CEL_METRICS_TOKEN is set, and then only to a
     * caller presenting it as a Bearer token; unset => 404 (the endpoint is
     * invisible). Front with a firewall / localhost bind for defense in depth. */
    if (portico_req_method_is(req, "GET") && portico_req_path_is(req, "/metrics")) {
        const char *mtok = getenv("CEL_METRICS_TOKEN");
        if (!mtok || !*mtok) return send_error(res, 404, "not found");
        size_t alen = 0;
        const char *auth = portico_req_header(req, "Authorization", &alen);
        const char *bearer = (auth && alen > 7 && strncasecmp(auth, "Bearer ", 7) == 0) ? auth + 7 : NULL;
        size_t blen = bearer ? alen - 7 : 0;
        size_t tlen = strlen(mtok);
        /* Constant-time compare: a short-circuiting memcmp leaks per-byte match
         * progress via timing, letting an attacker recover the token (L-2). */
        if (!bearer || blen != tlen || sodium_memcmp(bearer, mtok, tlen) != 0)
            return send_error(res, 401, "unauthorized");
        char *body = cel_metrics_render();
        if (!body) return send_error(res, 500, "metrics unavailable");
        portico_res_status(res, 200);
        portico_res_body(res, body, strlen(body), "text/plain; version=0.0.4");
        free(body);
        return 200;
    }

    /* GET /schema (resolve identity only for routes that need it — not assets/login) */
    if (portico_req_method_is(req, "GET") && portico_req_path_is(req, "/schema")) {
        cel_identity_t who;
        identity_from_request(req, &who);
        return send_api(res, cel_api_schema(&who));
    }

    /* GET /openapi.json — OpenAPI 3.0 spec generated from the live catalog. Gated
     * like /schema (any authenticated caller), since it exposes the same schema. */
    if (portico_req_method_is(req, "GET") && portico_req_path_is(req, "/openapi.json")) {
        cel_identity_t who;
        identity_from_request(req, &who);
        if (!who.authenticated) return send_error(res, 401, "authentication required");
        cJSON *doc = cel_openapi_build(cel_catalog_active(), cel_metrics_version());
        char *s = doc ? cJSON_PrintUnformatted(doc) : NULL;
        cJSON_Delete(doc);
        if (!s) return send_error(res, 500, "openapi unavailable");
        portico_res_status(res, 200);
        portico_res_body(res, s, strlen(s), "application/json");
        free(s);
        return 200;
    }

    /* POST /auth/login */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/auth/login")) {
        /* Throttle the expensive (Argon2id) auth path per client IP. */
        if (!cel_ratelimit_allow(g_auth_rl, portico_req_client_ip(req))) {
            cel_metric_inc(CEL_M_RATELIMITED);
            return send_error(res, 429, "too many requests");
        }
        cJSON *body = cJSON_ParseWithLength(req->body, req->body_len);
        if (!body) return send_error(res, 400, "invalid JSON");
        int st = send_api(res, cel_api_login(body));
        cJSON_Delete(body);
        return st;
    }

    /* POST /auth/register — public self-service signup (gated by role config) */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/auth/register")) {
        if (!cel_ratelimit_allow(g_auth_rl, portico_req_client_ip(req))) {
            cel_metric_inc(CEL_M_RATELIMITED);
            return send_error(res, 429, "too many requests");
        }
        cJSON *body = cJSON_ParseWithLength(req->body, req->body_len);
        if (!body) return send_error(res, 400, "invalid JSON");
        int st = send_api(res, cel_api_register(body));
        cJSON_Delete(body);
        return st;
    }

    /* POST /auth/users — admin-provisioned account creation (superuser only) */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/auth/users")) {
        cel_identity_t who;
        identity_from_request(req, &who);
        cJSON *body = cJSON_ParseWithLength(req->body, req->body_len);
        if (!body) return send_error(res, 400, "invalid JSON");
        int st = send_api(res, cel_api_create_user(&who, body));
        cJSON_Delete(body);
        return st;
    }

    /* POST /auth/mfa/enroll — start TOTP enrollment for the authenticated caller */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/auth/mfa/enroll")) {
        cel_identity_t who;
        identity_from_request(req, &who);
        return send_api(res, cel_api_mfa_enroll(&who));
    }

    /* POST /auth/mfa/confirm — finish enrollment by proving one code */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/auth/mfa/confirm")) {
        cel_identity_t who;
        identity_from_request(req, &who);
        cJSON *body = cJSON_ParseWithLength(req->body, req->body_len);
        if (!body) return send_error(res, 400, "invalid JSON");
        int st = send_api(res, cel_api_mfa_confirm(&who, body));
        cJSON_Delete(body);
        return st;
    }

    /* POST /auth/mfa/disable — turn off 2FA (requires a valid current code) */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/auth/mfa/disable")) {
        cel_identity_t who;
        identity_from_request(req, &who);
        cJSON *body = cJSON_ParseWithLength(req->body, req->body_len);
        if (!body) return send_error(res, 400, "invalid JSON");
        int st = send_api(res, cel_api_mfa_disable(&who, body));
        cJSON_Delete(body);
        return st;
    }

    /* POST /auth/mfa/recovery-codes — regenerate one-time recovery codes (Bearer) */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/auth/mfa/recovery-codes")) {
        cel_identity_t who;
        identity_from_request(req, &who);
        cJSON *body = cJSON_ParseWithLength(req->body, req->body_len);
        if (!body) return send_error(res, 400, "invalid JSON");
        int st = send_api(res, cel_api_mfa_recovery(&who, body));
        cJSON_Delete(body);
        return st;
    }

    /* POST /auth/mfa/verify — the second login step (public; throttled like login) */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/auth/mfa/verify")) {
        if (!cel_ratelimit_allow(g_auth_rl, portico_req_client_ip(req))) {
            cel_metric_inc(CEL_M_RATELIMITED);
            return send_error(res, 429, "too many requests");
        }
        cJSON *body = cJSON_ParseWithLength(req->body, req->body_len);
        if (!body) return send_error(res, 400, "invalid JSON");
        int st = send_api(res, cel_api_mfa_verify(body));
        cJSON_Delete(body);
        return st;
    }

    /* POST /auth/oauth — federated sign-in: verify a provider ID token, find/link
     * the identity, issue a session (public; throttled like login). */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/auth/oauth")) {
        if (!cel_ratelimit_allow(g_auth_rl, portico_req_client_ip(req))) {
            cel_metric_inc(CEL_M_RATELIMITED);
            return send_error(res, 429, "too many requests");
        }
        cJSON *body = cJSON_ParseWithLength(req->body, req->body_len);
        if (!body) return send_error(res, 400, "invalid JSON");
        int st = send_api(res, cel_api_oauth(body));
        cJSON_Delete(body);
        return st;
    }

    /* POST /auth/verify-email — redeem an email-verification token (public) */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/auth/verify-email")) {
        cJSON *body = cJSON_ParseWithLength(req->body, req->body_len);
        if (!body) return send_error(res, 400, "invalid JSON");
        int st = send_api(res, cel_api_verify_email(body));
        cJSON_Delete(body);
        return st;
    }

    /* POST /auth/verify-email/resend — re-send the verification email (Bearer;
     * throttled, since it sends mail) */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/auth/verify-email/resend")) {
        if (!cel_ratelimit_allow(g_auth_rl, portico_req_client_ip(req))) {
            cel_metric_inc(CEL_M_RATELIMITED);
            return send_error(res, 429, "too many requests");
        }
        cel_identity_t who;
        identity_from_request(req, &who);
        return send_api(res, cel_api_verify_email_resend(&who));
    }

    /* POST /auth/password/forgot — email a reset link (public; throttled, and it
     * sends mail). Always 200 so it can't be used to probe which emails exist. */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/auth/password/forgot")) {
        if (!cel_ratelimit_allow(g_auth_rl, portico_req_client_ip(req))) {
            cel_metric_inc(CEL_M_RATELIMITED);
            return send_error(res, 429, "too many requests");
        }
        cJSON *body = cJSON_ParseWithLength(req->body, req->body_len);
        if (!body) return send_error(res, 400, "invalid JSON");
        int st = send_api(res, cel_api_password_forgot(body));
        cJSON_Delete(body);
        return st;
    }

    /* POST /auth/password/reset — redeem a reset token + set a new password */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/auth/password/reset")) {
        if (!cel_ratelimit_allow(g_auth_rl, portico_req_client_ip(req))) {
            cel_metric_inc(CEL_M_RATELIMITED);
            return send_error(res, 429, "too many requests");
        }
        cJSON *body = cJSON_ParseWithLength(req->body, req->body_len);
        if (!body) return send_error(res, 400, "invalid JSON");
        int st = send_api(res, cel_api_password_reset(body));
        cJSON_Delete(body);
        return st;
    }

    /* POST /rpc/<fn> — call a whitelisted Postgres function (body = args object) */
    if (portico_req_method_is(req, "POST") && req->path_len > 5 && memcmp(req->path, "/rpc/", 5) == 0) {
        cel_identity_t who;
        identity_from_request(req, &who);
        /* Data-API throttle: per authenticated user when known, else per IP. */
        if (!cel_ratelimit_allow(g_api_rl, who.user_id[0] ? who.user_id : portico_req_client_ip(req)))
            return send_error(res, 429, "too many requests");
        char fn[64];
        if (copy_str(fn, sizeof fn, req->path + 5, req->path_len - 5) != 0)
            return send_error(res, 414, "function name too long");
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "fn", fn);
        if (req->body_len > 0) {
            cJSON *args = cJSON_ParseWithLength(req->body, req->body_len);
            if (!cJSON_IsObject(args)) { cJSON_Delete(args); cJSON_Delete(r);
                return send_error(res, 400, "args must be a JSON object"); }
            cJSON_AddItemToObject(r, "args", args);   /* takes ownership */
        }
        int st = send_api(res, cel_api_rpc(&who, r));
        cJSON_Delete(r);
        return st;
    }

    /* POST /sync/pull — offline-first delta pull: rows changed since a rev cursor
     * across the caller's syncable tables (engine built-in, not a hooks.lua rpc).
     * Body: { since, tables?, limit? }. */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/sync/pull")) {
        cel_identity_t who;
        identity_from_request(req, &who);
        if (!cel_ratelimit_allow(g_api_rl, who.user_id[0] ? who.user_id : portico_req_client_ip(req)))
            return send_error(res, 429, "too many requests");
        cJSON *body = req->body_len > 0 ? cJSON_ParseWithLength(req->body, req->body_len)
                                        : cJSON_CreateObject();
        if (!cJSON_IsObject(body)) { cJSON_Delete(body); return send_error(res, 400, "invalid JSON"); }
        int st = send_api(res, cel_api_sync_pull(&who, body));
        cJSON_Delete(body);
        return st;
    }

    /* POST /sync/push — offline-first delta push: a batch of client mutations,
     * applied all-or-nothing with per-row results (LWW + optional resolve hook). */
    if (portico_req_method_is(req, "POST") && portico_req_path_is(req, "/sync/push")) {
        cel_identity_t who;
        identity_from_request(req, &who);
        if (!cel_ratelimit_allow(g_api_rl, who.user_id[0] ? who.user_id : portico_req_client_ip(req)))
            return send_error(res, 429, "too many requests");
        cJSON *body = req->body_len > 0 ? cJSON_ParseWithLength(req->body, req->body_len) : NULL;
        if (!cJSON_IsObject(body)) { cJSON_Delete(body); return send_error(res, 400, "invalid JSON"); }
        int st = send_api(res, cel_api_sync_push(&who, body));
        cJSON_Delete(body);
        return st;
    }

    /* /api/<table>[/<id>] */
    if (req->path_len > 5 && memcmp(req->path, "/api/", 5) == 0) {
        cel_identity_t who;
        identity_from_request(req, &who);
        /* Data-API throttle: per authenticated user when known, else per IP. */
        if (!cel_ratelimit_allow(g_api_rl, who.user_id[0] ? who.user_id : portico_req_client_ip(req)))
            return send_error(res, 429, "too many requests");
        const char *rest = req->path + 5;
        size_t rest_len = req->path_len - 5;
        const char *slash = memchr(rest, '/', rest_len);

        char table[64];
        size_t table_len = slash ? (size_t)(slash - rest) : rest_len;
        if (copy_str(table, sizeof table, rest, table_len) != 0)
            return send_error(res, 414, "table name too long");

        if (slash) {
            /* /api/<table>/<id> : GET | PATCH/PUT | DELETE */
            char idbuf[256];
            size_t id_len = rest_len - table_len - 1;
            if (id_len == 0 || copy_str(idbuf, sizeof idbuf, slash + 1, id_len) != 0)
                return send_error(res, 400, "bad id");
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "table", table);
            cJSON_AddStringToObject(r, "id", idbuf);

            int st;
            if (portico_req_method_is(req, "GET")) {
                st = send_api(res, cel_api_get(&who, r));
            } else if (portico_req_method_is(req, "PATCH") || portico_req_method_is(req, "PUT")) {
                cJSON *values = cJSON_ParseWithLength(req->body, req->body_len);
                if (!cJSON_IsObject(values)) { cJSON_Delete(values); cJSON_Delete(r);
                    return send_error(res, 400, "invalid JSON body"); }
                cJSON_AddItemToObject(r, "values", values);   /* takes ownership */
                st = send_api(res, cel_api_update(&who, r));
            } else if (portico_req_method_is(req, "DELETE")) {
                st = send_api(res, cel_api_delete(&who, r));
            } else {
                st = send_error(res, 405, "method not allowed");
            }
            cJSON_Delete(r);
            return st;
        }

        /* /api/<table> : GET (list) | POST (create) */
        int st;
        if (portico_req_method_is(req, "GET")) {
            cJSON *r = build_list_req(table, req->query, req->query_len);
            st = send_api(res, cel_api_list(&who, r));
            cJSON_Delete(r);
        } else if (portico_req_method_is(req, "POST")) {
            cJSON *values = cJSON_ParseWithLength(req->body, req->body_len);
            if (!cJSON_IsObject(values)) { cJSON_Delete(values);
                return send_error(res, 400, "invalid JSON body"); }
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "table", table);
            cJSON_AddItemToObject(r, "values", values);       /* takes ownership */
            st = send_api(res, cel_api_create(&who, r));
            cJSON_Delete(r);
        } else {
            st = send_error(res, 405, "method not allowed");
        }
        return st;
    }

    /* per-bundle front-end: serve the current app's public/ for GET/HEAD, AFTER
     * the API routes (so /api, /auth, /rpc are never shadowed) but BEFORE the
     * embedded admin UI — an app with its own public/ owns its site. An SPA
     * fallback serves index.html for unmatched client-side routes (/admin,
     * /profile/:id, …). Apps without a public/ dir fall through to the admin UI. */
    if (portico_req_method_is(req, "GET") || portico_req_method_is(req, "HEAD")) {
        const cel_app_t *app = cel_apps_current();
        char pub[1100];
        if (app && app->bundle_dir[0] &&
            (size_t)snprintf(pub, sizeof pub, "%s/public", app->bundle_dir) < sizeof pub) {
            struct stat st;
            if (stat(pub, &st) == 0 && S_ISDIR(st.st_mode)) {
                portico_static_opts_t opts = {
                    .docroot = pub, .url_prefix = NULL, .index = "index.html",
                    .cache_control = NULL, .spa_fallback = "index.html", .dir_redirect = 1 };
                if (portico_res_static(res, req, &opts) == 0) return 200;   /* served (2xx/3xx) */
                return 404;   /* e.g. a traversal 403 — counts in the 4xx bucket */
            }
        }
    }

    /* embedded admin UI: serve static assets for GET paths (after the API routes) */
    if (portico_req_method_is(req, "GET") && req->path_len < 512) {
        char p[512];
        memcpy(p, req->path, req->path_len);
        p[req->path_len] = '\0';
        const cel_asset_t *a = cel_asset_find(p);
        if (a) {
            portico_res_status(res, 200);
            portico_res_body(res, a->data, a->len, a->ctype);
            return 200;
        }
    }

    return send_error(res, 404, "not found");
}

/* Emit the CORS response headers for an allowed origin. On a preflight, also the
 * allowed methods/headers and a cache lifetime. */
static void add_cors_headers(portico_response_t *res, const char *allow_origin, bool preflight) {
    portico_res_header(res, "Access-Control-Allow-Origin", allow_origin);
    portico_res_header(res, "Vary", "Origin");
    if (cel_cors_allow_credentials())
        portico_res_header(res, "Access-Control-Allow-Credentials", "true");
    if (preflight) {
        portico_res_header(res, "Access-Control-Allow-Methods", "GET, POST, PATCH, PUT, DELETE, OPTIONS");
        portico_res_header(res, "Access-Control-Allow-Headers", "Authorization, Content-Type");
        portico_res_header(res, "Access-Control-Max-Age", "600");
    }
}

int cel_http_router(const portico_request_t *req, portico_response_t *res, void *user_data) {
    (void)user_data;

    /* CORS: resolve the allowed origin (NULL = not allowed / CORS off => no headers).
     * `origin` is held at function scope: cel_cors_allow_origin may return a pointer
     * into it (the wildcard+credentials echo case), so it must outlive the response. */
    const char *allow_origin = NULL;
    char origin[256];
    if (cel_cors_enabled()) {
        size_t olen = 0;
        const char *oh = portico_req_header(req, "Origin", &olen);
        if (oh && olen > 0 && olen < sizeof origin) {
            memcpy(origin, oh, olen);
            origin[olen] = '\0';
            allow_origin = cel_cors_allow_origin(origin);
        }
    }

    /* Preflight: answer OPTIONS directly (no routing, no DB). */
    if (portico_req_method_is(req, "OPTIONS")) {
        if (allow_origin) add_cors_headers(res, allow_origin, true);
        portico_res_status(res, 204);
        portico_res_body(res, "", 0, "text/plain");
        cel_metric_inc(CEL_M_HTTP_2XX);
        return 0;
    }

    /* Reject an oversized body before it reaches the JSON parser (CPU/memory DoS). */
    if (g_max_body && req->body_len > g_max_body) {
        send_error(res, 413, "request body too large");
        if (allow_origin) add_cors_headers(res, allow_origin, false);
        cel_metric_inc(CEL_M_HTTP_4XX);
        return 0;
    }

    /* Route to the app named by the Host header (the request's app, bound to this
     * thread for the call). /health and /metrics are process-level and need no app;
     * for any other path an unknown Host is a hard 404 — never serve another app. */
    size_t hlen = 0;
    const char *host = portico_req_header(req, "Host", &hlen);
    char hostbuf[256] = {0};
    if (host && hlen < sizeof hostbuf) { memcpy(hostbuf, host, hlen); hostbuf[hlen] = '\0'; }
    cel_app_t *app = cel_apps_resolve(hostbuf[0] ? hostbuf : NULL);
    if (!app && !portico_req_path_is(req, "/health") && !portico_req_path_is(req, "/metrics")) {
        send_error(res, 404, "unknown app");
        if (allow_origin) add_cors_headers(res, allow_origin, false);
        cel_metric_inc(CEL_M_HTTP_4XX);
        return 0;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    cel_apps_enter(app);
    int status = route(req, res);
    cel_apps_leave();

    if (allow_origin) add_cors_headers(res, allow_origin, false);   /* on the actual response */

    clock_gettime(CLOCK_MONOTONIC, &t1);
    cel_metric_http_observe((double)(t1.tv_sec - t0.tv_sec) +
                            (double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
    if      (status >= 500) cel_metric_inc(CEL_M_HTTP_5XX);
    else if (status >= 400) cel_metric_inc(CEL_M_HTTP_4XX);
    else                    cel_metric_inc(CEL_M_HTTP_2XX);
    return 0;
}
