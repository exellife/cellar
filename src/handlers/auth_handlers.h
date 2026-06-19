/* cellar — auth opcode handlers (run on POOL_DB). */
#ifndef CEL_AUTH_HANDLERS_H
#define CEL_AUTH_HANDLERS_H

#include "opcode_dispatcher.h"

handler_result_t cel_handle_login(opcode_context_t *ctx);
handler_result_t cel_handle_verify_session(opcode_context_t *ctx);
handler_result_t cel_handle_logout(opcode_context_t *ctx);

#endif /* CEL_AUTH_HANDLERS_H */
