/* pgforge — generic schema-driven data-layer handlers (read path). */
#ifndef PGF_DATA_HANDLERS_H
#define PGF_DATA_HANDLERS_H

#include "opcode_dispatcher.h"

handler_result_t pgf_handle_db_list(opcode_context_t *ctx);    /* OP_DB_LIST   */
handler_result_t pgf_handle_db_get(opcode_context_t *ctx);     /* OP_DB_GET    */
handler_result_t pgf_handle_db_create(opcode_context_t *ctx);  /* OP_DB_CREATE */
handler_result_t pgf_handle_db_update(opcode_context_t *ctx);  /* OP_DB_UPDATE */
handler_result_t pgf_handle_db_delete(opcode_context_t *ctx);  /* OP_DB_DELETE */
handler_result_t pgf_handle_db_rpc(opcode_context_t *ctx);     /* OP_DB_RPC    */
handler_result_t pgf_handle_db_query(opcode_context_t *ctx);   /* OP_DB_QUERY  */

#endif /* PGF_DATA_HANDLERS_H */
