#include "realtime_handlers.h"
#include "respond.h"
#include "engine/api.h"
#include "engine/policy.h"
#include "engine/realtime.h"

#include <stdint.h>
#include <cjson/cJSON.h>

/* The connection fd is stashed in ctx->user_data by the frame dispatcher. */
static int ctx_fd(const opcode_context_t *ctx) { return (int)(intptr_t)ctx->user_data; }

handler_result_t cel_handle_subscribe(opcode_context_t *ctx) {
    cJSON *req = cJSON_ParseWithLength((const char *)ctx->data, ctx->data_size);
    if (!req) return cel_respond_error(ctx, "invalid JSON");

    const cJSON *tk = cJSON_GetObjectItemCaseSensitive(req, "token");
    cel_identity_t who;
    cel_identity_from_token(cJSON_IsString(tk) ? tk->valuestring : NULL, &who);

    cel_subscription_t sub;
    char err[256] = {0};
    int status = cel_api_authorize_subscription(&who, req, &sub, err, sizeof err);
    cJSON_Delete(req);
    if (status != 200) return cel_respond_error(ctx, err[0] ? err : "subscribe denied");

    if (cel_realtime_subscribe(ctx_fd(ctx), &sub) != 0)
        return cel_respond_error(ctx, "subscribe failed");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "subscribed", sub.table);
    return cel_respond_json(ctx, o, false);
}

handler_result_t cel_handle_unsubscribe(opcode_context_t *ctx) {
    cJSON *req = cJSON_ParseWithLength((const char *)ctx->data, ctx->data_size);
    if (!req) return cel_respond_error(ctx, "invalid JSON");
    const cJSON *tbl = cJSON_GetObjectItemCaseSensitive(req, "table");
    if (!cJSON_IsString(tbl)) { cJSON_Delete(req); return cel_respond_error(ctx, "table required"); }

    cel_realtime_unsubscribe(ctx_fd(ctx), tbl->valuestring);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "unsubscribed", tbl->valuestring);
    cJSON_Delete(req);
    return cel_respond_json(ctx, o, false);
}
