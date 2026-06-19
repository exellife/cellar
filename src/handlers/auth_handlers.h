/* pgforge — auth opcode handlers (run on POOL_DB). */
#ifndef PGF_AUTH_HANDLERS_H
#define PGF_AUTH_HANDLERS_H

#include "opcode_dispatcher.h"

handler_result_t pgf_handle_login(opcode_context_t *ctx);
handler_result_t pgf_handle_verify_session(opcode_context_t *ctx);
handler_result_t pgf_handle_logout(opcode_context_t *ctx);

#endif /* PGF_AUTH_HANDLERS_H */
