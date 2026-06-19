/* cellar — realtime pub/sub opcode handlers.
 * SUBSCRIBE verifies a token + access rules (POOL_DB); UNSUBSCRIBE is local
 * registry-only (POOL_INLINE). CHANGE events are pushed by the engine, not here. */
#ifndef CEL_REALTIME_HANDLERS_H
#define CEL_REALTIME_HANDLERS_H

#include "opcode_dispatcher.h"

handler_result_t cel_handle_subscribe(opcode_context_t *ctx);
handler_result_t cel_handle_unsubscribe(opcode_context_t *ctx);

#endif /* CEL_REALTIME_HANDLERS_H */
