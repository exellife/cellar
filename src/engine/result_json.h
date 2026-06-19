/* ============================================================================
 * cellar — serialize a SQLite result (a stepped sqlite3_stmt) to typed JSON.
 *
 * The SQLite-era replacement for row_json.c's PGresult serializer (design §10:
 * "PGresult-equivalent → typed JSON"). Same output contract as the Postgres
 * path so the REST API shape is unchanged: integers/reals become JSON numbers,
 * declared booleans become true/false, JSON columns are parsed, NULLs become
 * null, everything else is a string.
 *
 * SQLite is dynamically typed (column *affinity*, not a fixed type), so the
 * declared catalog type drives serialization where we have it — a column the
 * app declared BOOLEAN comes back as a JSON bool even though SQLite stored 0/1.
 * With no catalog (RPC / ad-hoc results) we fall back to the value's runtime
 * storage class.
 *
 * Each function STEPS the statement to completion (consuming the cursor); the
 * caller still owns the statement and must sqlite3_reset/finalize it.
 * ============================================================================ */
#ifndef CEL_RESULT_JSON_H
#define CEL_RESULT_JSON_H

#include "schema_catalog.h"     /* cel_table_t, cel_coltype_t — libpq-free */
#include <sqlite3.h>
#include <cjson/cJSON.h>

/* Build a JSON array of row objects, typing each column from `t` by name.
 * Columns absent from the catalog (e.g. an aliased aggregate) fall back to the
 * value's runtime storage class. */
cJSON *cel_stmt_rows_to_json(sqlite3_stmt *st, const cel_table_t *t);

/* Like the above but with no catalog: every column is typed from its runtime
 * storage class. Used for RPC and other results whose shape isn't a known table. */
cJSON *cel_stmt_result_to_json(sqlite3_stmt *st);

#endif /* CEL_RESULT_JSON_H */
