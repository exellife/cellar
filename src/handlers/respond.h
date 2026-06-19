/* cellar — shared helper to write a JSON object as an opcode response. */
#ifndef CEL_RESPOND_H
#define CEL_RESPOND_H

#include "opcode_dispatcher.h"
#include <cjson/cJSON.h>
#include <stdbool.h>

/* Serialize `obj` into ctx->response_data (owned). Consumes `obj`.
 * When is_error, sets the protocol error flag. */
handler_result_t cel_respond_json(opcode_context_t *ctx, cJSON *obj, bool is_error);

/* Convenience: {"status":"error","message":<message>} with the error flag. */
handler_result_t cel_respond_error(opcode_context_t *ctx, const char *message);

#endif /* CEL_RESPOND_H */
