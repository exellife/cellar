/* pgforge — shared helper to write a JSON object as an opcode response. */
#ifndef PGF_RESPOND_H
#define PGF_RESPOND_H

#include "opcode_dispatcher.h"
#include <cjson/cJSON.h>
#include <stdbool.h>

/* Serialize `obj` into ctx->response_data (owned). Consumes `obj`.
 * When is_error, sets the protocol error flag. */
handler_result_t pgf_respond_json(opcode_context_t *ctx, cJSON *obj, bool is_error);

/* Convenience: {"status":"error","message":<message>} with the error flag. */
handler_result_t pgf_respond_error(opcode_context_t *ctx, const char *message);

#endif /* PGF_RESPOND_H */
