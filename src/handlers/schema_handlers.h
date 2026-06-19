/* pgforge — schema opcode handlers. */
#ifndef PGF_SCHEMA_HANDLERS_H
#define PGF_SCHEMA_HANDLERS_H

#include "opcode_dispatcher.h"

/* OP_DB_SCHEMA: return the active schema catalog as JSON. Reads in-memory state
 * only (no DB round-trip), so it runs on POOL_INLINE. */
handler_result_t pgf_handle_db_schema(opcode_context_t *ctx);

#endif /* PGF_SCHEMA_HANDLERS_H */
