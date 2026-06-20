/* ============================================================================
 * cellar — per-app schema catalog via SQLite introspection (design §5).
 *
 * The SQLite-era replacement for schema_catalog.c's information_schema queries.
 * Builds the same cel_catalog_t (so the rest of the engine and cel_catalog_free
 * are unchanged) from:
 *   - sqlite_master           → the user table list (sqlite_* / cel_* / _% excluded);
 *   - pragma_table_info(t)     → columns: declared type, NOT NULL, default, PK;
 *   - pragma_foreign_key_list  → FK target table/column.
 *
 * SQLite is dynamically typed, so a column's engine type is derived from its
 * *declared* type's affinity (CREATE TABLE ... INTEGER/TEXT/BOOLEAN/JSON/...),
 * mirroring how the serializer reads it back. libpq-free.
 * ============================================================================ */
#include "schema_catalog.h"
#include "logger.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>    /* strcasestr (GNU; _GNU_SOURCE is set project-wide) */

/* ---- declared-type → normalized engine type (SQLite affinity) -------------- */
/* Specific semantic types first (so BOOLEAN/JSON/DATE aren't swallowed by the
 * generic affinity ladder), then SQLite's own affinity rules, default OTHER —
 * OTHER serializes by runtime storage class, the safe choice for odd types. */
static cel_coltype_t affinity_from_decl(const char *decl) {
    if (!decl || !decl[0])                 return CEL_T_OTHER;   /* no type → BLOB/NONE */
    if (strcasestr(decl, "bool"))          return CEL_T_BOOL;
    if (strcasestr(decl, "json"))          return CEL_T_JSON;
    if (strcasestr(decl, "uuid"))          return CEL_T_UUID;
    if (strcasestr(decl, "datetime") ||
        strcasestr(decl, "timestamp"))     return CEL_T_TIMESTAMPTZ;
    if (strcasestr(decl, "date"))          return CEL_T_DATE;
    if (strcasestr(decl, "int"))           return CEL_T_BIGINT;  /* SQLite ints are 64-bit */
    if (strcasestr(decl, "char") ||
        strcasestr(decl, "clob") ||
        strcasestr(decl, "text"))          return CEL_T_TEXT;
    if (strcasestr(decl, "real") ||
        strcasestr(decl, "floa") ||
        strcasestr(decl, "doub"))          return CEL_T_FLOAT;
    if (strcasestr(decl, "num") ||
        strcasestr(decl, "dec"))           return CEL_T_NUMERIC;
    if (strcasestr(decl, "blob"))          return CEL_T_OTHER;
    return CEL_T_OTHER;
}

/* ---- small catalog builders (static; the public free/find live in the PG TU) */
static cel_table_t *add_table(cel_catalog_t *cat, const char *name) {
    cel_table_t *grown = realloc(cat->tables, (cat->ntables + 1) * sizeof *cat->tables);
    if (!grown) return NULL;
    cat->tables = grown;
    cel_table_t *t = &cat->tables[cat->ntables++];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->pk_index = -1;
    return t;
}
static cel_table_t *find_table(cel_catalog_t *cat, const char *name) {
    for (int i = 0; i < cat->ntables; i++)
        if (!strcmp(cat->tables[i].name, name)) return &cat->tables[i];
    return NULL;
}
static cel_column_t *add_column(cel_table_t *t, const char *name) {
    cel_column_t *grown = realloc(t->cols, (t->ncols + 1) * sizeof *t->cols);
    if (!grown) return NULL;
    t->cols = grown;
    cel_column_t *c = &t->cols[t->ncols++];
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "%s", name);
    return c;
}
static cel_column_t *find_column(cel_table_t *t, const char *name) {
    for (int i = 0; i < t->ncols; i++)
        if (!strcmp(t->cols[i].name, name)) return &t->cols[i];
    return NULL;
}

/* mirror of cel_catalog_free, for our own error paths (the public one lives in
 * the Postgres TU and isn't linked into libpq-free unit tests). */
static void free_catalog(cel_catalog_t *cat) {
    if (!cat) return;
    for (int i = 0; i < cat->ntables; i++) free(cat->tables[i].cols);
    free(cat->tables);
    free(cat);
}

/* ---- one table's columns + foreign keys ------------------------------------ */
static int load_columns(sqlite3 *db, cel_table_t *t) {
    sqlite3_stmt *st = NULL;
    /* pragma_table_info is a table-valued function → bind the name (injection-safe) */
    if (sqlite3_prepare_v2(db,
            "SELECT name, type, \"notnull\", dflt_value, pk "
            "FROM pragma_table_info(?1)", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, t->name, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        const char *decl = (const char *)sqlite3_column_text(st, 1);
        cel_column_t *c = add_column(t, name ? name : "");
        if (!c) { sqlite3_finalize(st); return -1; }
        snprintf(c->decl_type, sizeof c->decl_type, "%s", decl ? decl : "");  /* raw declared type */
        c->type        = affinity_from_decl(decl);
        c->nullable    = sqlite3_column_int(st, 2) == 0;          /* notnull==0 → nullable */
        c->has_default = sqlite3_column_type(st, 3) != SQLITE_NULL;
        int pkpos      = sqlite3_column_int(st, 4);               /* 0=no, >=1 position in PK */
        if (pkpos > 0) {
            c->is_pk = true;
            if (pkpos == 1) t->pk_index = t->ncols - 1;           /* first PK column */
        }
    }
    sqlite3_finalize(st);
    return 0;
}

static void load_foreign_keys(sqlite3 *db, cel_table_t *t) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT \"from\", \"table\", \"to\", \"on_delete\" "
            "FROM pragma_foreign_key_list(?1)", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, t->name, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *from  = (const char *)sqlite3_column_text(st, 0);
        const char *ftab  = (const char *)sqlite3_column_text(st, 1);
        const char *fcol  = (const char *)sqlite3_column_text(st, 2);
        const char *ondel = (const char *)sqlite3_column_text(st, 3);
        cel_column_t *c = from ? find_column(t, from) : NULL;
        if (!c) continue;
        c->is_fk = true;
        snprintf(c->fk_table,  sizeof c->fk_table,  "%s", ftab ? ftab : "");
        snprintf(c->fk_column, sizeof c->fk_column, "%s", fcol ? fcol : "");
        c->fk_cascade = ondel && strcasecmp(ondel, "CASCADE") == 0;
    }
    sqlite3_finalize(st);
}

cel_catalog_t *cel_catalog_build_sqlite(sqlite3 *db) {
    if (!db) return NULL;
    cel_catalog_t *cat = calloc(1, sizeof *cat);
    if (!cat) return NULL;

    /* user tables only: drop SQLite internals, cellar's own cel_* tables, and any
     * _%-prefixed table — a leading underscore is reserved for engine-internal
     * tables (e.g. _sync_seq for sync, future _hooks), never REST-exposed. */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT name FROM sqlite_master "
            "WHERE type='table' "
            "  AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' "
            "  AND name NOT LIKE 'cel\\_%' ESCAPE '\\' "
            "  AND name NOT LIKE '\\_%' ESCAPE '\\' "
            "ORDER BY name", -1, &st, NULL) != SQLITE_OK) {
        LOG_ERROR("catalog(sqlite): table list query failed: %s", sqlite3_errmsg(db));
        free_catalog(cat);
        return NULL;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        if (!name) continue;
        cel_table_t *t = add_table(cat, name);
        if (!t) { sqlite3_finalize(st); free_catalog(cat); return NULL; }
    }
    sqlite3_finalize(st);

    /* columns + FKs per table (find_table re-resolves because add_table may have
     * realloc'd the table array out from under an earlier pointer) */
    for (int i = 0; i < cat->ntables; i++) {
        cel_table_t *t = &cat->tables[i];
        if (load_columns(db, t) != 0) {
            LOG_ERROR("catalog(sqlite): table_info(%s) failed: %s", t->name, sqlite3_errmsg(db));
            free_catalog(cat);
            return NULL;
        }
        /* sync opt-in is detect-by-columns: a table carrying both `rev` and
         * `deleted` columns is syncable (the engine stamps rev + tombstones it). */
        t->syncable = find_column(t, "rev") != NULL && find_column(t, "deleted") != NULL;
    }
    for (int i = 0; i < cat->ntables; i++)
        load_foreign_keys(db, &cat->tables[i]);

    LOG_INFO("catalog(sqlite): %d table(s) introspected", cat->ntables);
    (void)find_table;   /* kept for symmetry with the PG builder; unused here */
    return cat;
}
