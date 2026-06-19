/* ============================================================================
 * pgforge — schema catalog
 *
 * An in-memory model of the database schema, introspected from information_schema
 * at startup. This is the foundation of the schema-driven engine: it determines
 * which tables are exposed, drives type-correct SQL binding and JSON typing, and
 * feeds the auto-generated admin UI.
 *
 * Internal pgforge tables (pgf_*) are excluded — they are infrastructure, not
 * user data, and must never be reachable through the generic data layer.
 * ============================================================================ */
#ifndef PGF_SCHEMA_CATALOG_H
#define PGF_SCHEMA_CATALOG_H

#include <stdbool.h>

/* Normalized column type. Maps many Postgres types onto a small set the engine
 * knows how to bind and serialize. PGF_T_OTHER is handled as text. */
typedef enum {
    PGF_T_INT,          /* int2 / int4 */
    PGF_T_BIGINT,       /* int8 */
    PGF_T_FLOAT,        /* float4 / float8 */
    PGF_T_NUMERIC,      /* numeric / decimal */
    PGF_T_BOOL,         /* bool */
    PGF_T_TEXT,         /* text / varchar / char / name */
    PGF_T_UUID,         /* uuid */
    PGF_T_TIMESTAMPTZ,  /* timestamptz / timestamp */
    PGF_T_DATE,         /* date */
    PGF_T_JSON,         /* json / jsonb */
    PGF_T_OTHER         /* anything else (treated as text) */
} pgf_coltype_t;

typedef struct {
    char          name[64];
    char          pg_type[64];   /* raw udt_name, e.g. "int4", "varchar" */
    pgf_coltype_t type;          /* normalized */
    bool          nullable;
    bool          has_default;
    bool          is_pk;
    bool          is_fk;
    char          fk_table[64];  /* valid when is_fk */
    char          fk_column[64]; /* valid when is_fk */
} pgf_column_t;

typedef struct {
    char          name[64];
    pgf_column_t *cols;
    int           ncols;
    int           pk_index;      /* index of single-column PK, or -1 */
} pgf_table_t;

typedef struct {
    pgf_table_t *tables;
    int          ntables;
} pgf_catalog_t;

/* Introspect the connected database into a fresh catalog. Acquires a pooled
 * connection internally. Returns NULL on failure. Caller owns the result. */
pgf_catalog_t *pgf_catalog_build(void);

void pgf_catalog_free(pgf_catalog_t *cat);

/* Look up a table by name (NULL if absent / not exposed). */
const pgf_table_t *pgf_catalog_find(const pgf_catalog_t *cat, const char *table);

/* Look up a column within a table by name (NULL if absent). */
const pgf_column_t *pgf_table_column(const pgf_table_t *t, const char *column);

/* Serialize the catalog to a malloc'd JSON string (caller frees). */
char *pgf_catalog_to_json(const pgf_catalog_t *cat);

/* Build the catalog as a cJSON object (caller owns / cJSON_Delete). */
struct cJSON *pgf_catalog_to_cjson(const pgf_catalog_t *cat);

/* String form of a normalized type (for JSON output / debugging). */
const char *pgf_coltype_name(pgf_coltype_t t);

/* Process-wide active catalog (built once at startup, read by handlers). */
void                 pgf_catalog_set_active(pgf_catalog_t *cat);
const pgf_catalog_t *pgf_catalog_active(void);

#endif /* PGF_SCHEMA_CATALOG_H */
