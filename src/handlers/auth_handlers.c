#include "auth_handlers.h"
#include "respond.h"
#include "engine/api.h"
#include "core/auth.h"

#include <cjson/cJSON.h>

static const char *req_str(const cJSON *req, const char *key) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(req, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

handler_result_t cel_handle_login(opcode_context_t *ctx) {
    cJSON *req = cJSON_ParseWithLength((const char *)ctx->data, ctx->data_size);
    if (!req) return cel_respond_error(ctx, "invalid JSON");
    cel_api_result_t res = cel_api_login(req);   /* shared with REST */
    cJSON_Delete(req);
    return cel_respond_json(ctx, res.body, res.http_status >= 400);
}

handler_result_t cel_handle_verify_session(opcode_context_t *ctx) {
    cJSON *req = cJSON_ParseWithLength((const char *)ctx->data, ctx->data_size);
    if (!req) return cel_respond_error(ctx, "invalid JSON");
    const char *token = req_str(req, "token");
    if (!token) { cJSON_Delete(req); return cel_respond_error(ctx, "token required"); }

    cel_user_t user;
    int rc = cel_auth_verify(token, &user);
    cJSON_Delete(req);
    if (rc == CEL_AUTH_INVALID) return cel_respond_error(ctx, "invalid or expired token");
    if (rc != CEL_AUTH_OK)      return cel_respond_error(ctx, "server error");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON *u = cJSON_AddObjectToObject(o, "user");
    cJSON_AddStringToObject(u, "id", user.id);
    cJSON_AddStringToObject(u, "email", user.email);
    cJSON_AddStringToObject(u, "role", user.role);
    cJSON_AddBoolToObject(u, "email_verified", user.email_verified);
    return cel_respond_json(ctx, o, false);
}

handler_result_t cel_handle_logout(opcode_context_t *ctx) {
    cJSON *req = cJSON_ParseWithLength((const char *)ctx->data, ctx->data_size);
    if (!req) return cel_respond_error(ctx, "invalid JSON");
    const char *token = req_str(req, "token");
    if (!token) { cJSON_Delete(req); return cel_respond_error(ctx, "token required"); }

    int rc = cel_auth_logout(token);
    cJSON_Delete(req);
    if (rc != CEL_AUTH_OK) return cel_respond_error(ctx, "server error");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    return cel_respond_json(ctx, o, false);
}
