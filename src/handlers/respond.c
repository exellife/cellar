#include "respond.h"
#include "core/protocol.h"

#include <stdlib.h>
#include <string.h>

handler_result_t pgf_respond_json(opcode_context_t *ctx, cJSON *obj, bool is_error) {
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) return RESULT_ERROR;

    size_t n = strlen(s);
    void *buf = malloc(n ? n : 1);
    if (!buf) { free(s); return RESULT_ERROR; }
    memcpy(buf, s, n);
    free(s);

    ctx->response_data = buf;
    ctx->response_size = n;
    ctx->owns_data     = true;
    if (is_error) ctx->flags |= PGF_FLAG_ERROR;
    return RESULT_SUCCESS;
}

handler_result_t pgf_respond_error(opcode_context_t *ctx, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "error");
    cJSON_AddStringToObject(o, "message", message);
    return pgf_respond_json(ctx, o, true);
}
