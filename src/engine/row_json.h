/* cellar — serialize a PGresult to JSON using catalog column types. */
#ifndef CEL_ROW_JSON_H
#define CEL_ROW_JSON_H

#include "schema_catalog.h"
#include <libpq-fe.h>
#include <cjson/cJSON.h>

/* Build a JSON array of row objects from a text-format PGresult. Column types
 * are looked up in `t` by field name; unknown fields fall back to string. */
cJSON *cel_rows_to_json(PGresult *res, const cel_table_t *t);

/* Like cel_rows_to_json but with no catalog table: column types are inferred
 * from each result field's Postgres type OID. Used for RPC, whose result shape
 * comes from a function's return type rather than a known table. */
cJSON *cel_result_to_json(PGresult *res);

#endif /* CEL_ROW_JSON_H */
