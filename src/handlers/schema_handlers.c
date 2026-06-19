#include "schema_handlers.h"
#include "respond.h"
#include "engine/api.h"
#include "engine/policy.h"

#include <cjson/cJSON.h>

handler_result_t cel_handle_db_schema(opcode_context_t *ctx) {
    /* schema requires auth: take the token from an optional JSON payload. */
    cJSON *req = cJSON_ParseWithLength((const char *)ctx->data, ctx->data_size);
    const cJSON *tk = req ? cJSON_GetObjectItemCaseSensitive(req, "token") : NULL;
    cel_identity_t who;
    cel_identity_from_token(cJSON_IsString(tk) ? tk->valuestring : NULL, &who);

    cel_api_result_t res = cel_api_schema(&who);
    if (req) cJSON_Delete(req);
    return cel_respond_json(ctx, res.body, res.http_status >= 400);
}
