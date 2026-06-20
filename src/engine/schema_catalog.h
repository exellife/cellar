/* ============================================================================
 * cellar — schema catalog
 *
 * An in-memory model of the database schema, introspected from information_schema
 * at startup. This is the foundation of the schema-driven engine: it determines
 * which tables are exposed, drives type-correct SQL binding and JSON typing, and
 * feeds the auto-generated admin UI.
 *
 * Internal tables (cel_*, sqlite_*, and any _%-prefixed table) are excluded —
 * they are infrastructure, not user data, and must never be reachable through
 * the generic data layer.
 * ============================================================================ */
#ifndef CEL_SCHEMA_CATALOG_H
#define CEL_SCHEMA_CATALOG_H

#include <stdbool.h>

/* Normalized column type. Maps many Postgres types onto a small set the engine
 * knows how to bind and serialize. CEL_T_OTHER is handled as text. */
typedef enum {
    CEL_T_INT,          /* int2 / int4 */
    CEL_T_BIGINT,       /* int8 */
    CEL_T_FLOAT,        /* float4 / float8 */
    CEL_T_NUMERIC,      /* numeric / decimal */
    CEL_T_BOOL,         /* bool */
    CEL_T_TEXT,         /* text / varchar / char / name */
    CEL_T_UUID,         /* uuid */
    CEL_T_TIMESTAMPTZ,  /* timestamptz / timestamp */
    CEL_T_DATE,         /* date */
    CEL_T_JSON,         /* json / jsonb */
    CEL_T_OTHER         /* anything else (treated as text) */
} cel_coltype_t;

typedef struct {
    char          name[64];
    char          decl_type[64];   /* raw SQLite declared type, e.g. "INTEGER", "UUID" */
    cel_coltype_t type;          /* normalized */
    bool          nullable;
    bool          has_default;
    bool          is_pk;
    bool          is_fk;
    char          fk_table[64];  /* valid when is_fk */
    char          fk_column[64]; /* valid when is_fk */
    bool          fk_cascade;    /* FK declared ON DELETE CASCADE (drives sync cascade soft-delete) */
} cel_column_t;

typedef struct {
    char          name[64];
    cel_column_t *cols;
    int           ncols;
    int           pk_index;      /* index of single-column PK, or -1 */
    bool          syncable;      /* opted into offline-first sync: has both a `rev`
                                  * and a `deleted` column (design: sync Appendix A).
                                  * The engine stamps `rev` + tombstones such tables. */
} cel_table_t;

typedef struct {
    cel_table_t *tables;
    int          ntables;
} cel_catalog_t;

/* Introspect an open SQLite database into a fresh catalog (per-app; design §5):
 * user tables from sqlite_master + PRAGMA table_info / foreign_key_list, column
 * types mapped from SQLite declared-type affinity. Internal cel_*, sqlite_*, and
 * _%-prefixed tables are excluded. A table with both `rev` and `deleted` columns
 * is flagged `syncable`. Returns NULL on failure. Caller owns the result
 * (cel_catalog_free). `sqlite3` is forward-declared so PG-only translation units
 * including this header don't need <sqlite3.h>. */
struct sqlite3;
cel_catalog_t *cel_catalog_build_sqlite(struct sqlite3 *db);

void cel_catalog_free(cel_catalog_t *cat);

/* Look up a table by name (NULL if absent / not exposed). */
const cel_table_t *cel_catalog_find(const cel_catalog_t *cat, const char *table);

/* Look up a column within a table by name (NULL if absent). */
const cel_column_t *cel_table_column(const cel_table_t *t, const char *column);

/* Serialize the catalog to a malloc'd JSON string (caller frees). */
char *cel_catalog_to_json(const cel_catalog_t *cat);

/* Build the catalog as a cJSON object (caller owns / cJSON_Delete). */
struct cJSON *cel_catalog_to_cjson(const cel_catalog_t *cat);

/* String form of a normalized type (for JSON output / debugging). */
const char *cel_coltype_name(cel_coltype_t t);

/* The catalog handlers read. cel_catalog_active() returns this thread's binding
 * (cel_catalog_set_active, by per-request routing) if set, else the process-wide
 * default (cel_catalog_set_default, set at boot for single-app), else NULL. */
void                 cel_catalog_set_active(cel_catalog_t *cat);   /* per-thread */
const cel_catalog_t *cel_catalog_active(void);
void                 cel_catalog_set_default(cel_catalog_t *cat);  /* process-wide */

#endif /* CEL_SCHEMA_CATALOG_H */
