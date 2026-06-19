#include "data_handlers.h"
#include "respond.h"
#include "engine/api.h"
#include "engine/policy.h"

#include <cjson/cJSON.h>

/* Thin WebSocket adapters over the transport-neutral engine API. Identity comes
 * from a "token" field in the request payload (verified per message). */

typedef pgf_api_result_t (*data_fn)(const pgf_identity_t *, const cJSON *);

static handler_result_t ws_data_op(opcode_context_t *ctx, data_fn fn) {
    cJSON *req = cJSON_ParseWithLength((const char *)ctx->data, ctx->data_size);
    if (!req) return pgf_respond_error(ctx, "invalid JSON");

    const cJSON *tk = cJSON_GetObjectItemCaseSensitive(req, "token");
    pgf_identity_t who;
    pgf_identity_from_token(cJSON_IsString(tk) ? tk->valuestring : NULL, &who);

    pgf_api_result_t res = fn(&who, req);
    cJSON_Delete(req);
    return pgf_respond_json(ctx, res.body, res.http_status >= 400);
}

handler_result_t pgf_handle_db_list(opcode_context_t *ctx)   { return ws_data_op(ctx, pgf_api_list); }
handler_result_t pgf_handle_db_get(opcode_context_t *ctx)    { return ws_data_op(ctx, pgf_api_get); }
handler_result_t pgf_handle_db_create(opcode_context_t *ctx) { return ws_data_op(ctx, pgf_api_create); }
handler_result_t pgf_handle_db_update(opcode_context_t *ctx) { return ws_data_op(ctx, pgf_api_update); }
handler_result_t pgf_handle_db_delete(opcode_context_t *ctx) { return ws_data_op(ctx, pgf_api_delete); }
handler_result_t pgf_handle_db_rpc(opcode_context_t *ctx)    { return ws_data_op(ctx, pgf_api_rpc); }
/* OP_DB_QUERY is the richer read (embed + count); pgf_api_list reads those fields. */
handler_result_t pgf_handle_db_query(opcode_context_t *ctx)  { return ws_data_op(ctx, pgf_api_list); }
