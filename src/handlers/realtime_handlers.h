/* pgforge — realtime pub/sub opcode handlers.
 * SUBSCRIBE verifies a token + access rules (POOL_DB); UNSUBSCRIBE is local
 * registry-only (POOL_INLINE). CHANGE events are pushed by the engine, not here. */
#ifndef PGF_REALTIME_HANDLERS_H
#define PGF_REALTIME_HANDLERS_H

#include "opcode_dispatcher.h"

handler_result_t pgf_handle_subscribe(opcode_context_t *ctx);
handler_result_t pgf_handle_unsubscribe(opcode_context_t *ctx);

#endif /* PGF_REALTIME_HANDLERS_H */
