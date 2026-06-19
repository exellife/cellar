#include "schema_catalog.h"
#include "db_connection.h"
#include "logger.h"

#include <libpq-fe.h>
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>

/* ---- type normalization ---------------------------------------------------- */

static pgf_coltype_t normalize_type(const char *udt) {
    if (!strcmp(udt, "int2") || !strcmp(udt, "int4"))         return PGF_T_INT;
    if (!strcmp(udt, "int8"))                                  return PGF_T_BIGINT;
    if (!strcmp(udt, "float4") || !strcmp(udt, "float8"))      return PGF_T_FLOAT;
    if (!strcmp(udt, "numeric"))                               return PGF_T_NUMERIC;
    if (!strcmp(udt, "bool"))                                  return PGF_T_BOOL;
    if (!strcmp(udt, "text") || !strcmp(udt, "varchar") ||
        !strcmp(udt, "bpchar") || !strcmp(udt, "name"))        return PGF_T_TEXT;
    if (!strcmp(udt, "uuid"))                                  return PGF_T_UUID;
    if (!strcmp(udt, "timestamptz") || !strcmp(udt, "timestamp")) return PGF_T_TIMESTAMPTZ;
    if (!strcmp(udt, "date"))                                  return PGF_T_DATE;
    if (!strcmp(udt, "json") || !strcmp(udt, "jsonb"))         return PGF_T_JSON;
    return PGF_T_OTHER;
}

const char *pgf_coltype_name(pgf_coltype_t t) {
    switch (t) {
        case PGF_T_INT:         return "int";
        case PGF_T_BIGINT:      return "bigint";
        case PGF_T_FLOAT:       return "float";
        case PGF_T_NUMERIC:     return "numeric";
        case PGF_T_BOOL:        return "bool";
        case PGF_T_TEXT:        return "text";
        case PGF_T_UUID:        return "uuid";
        case PGF_T_TIMESTAMPTZ: return "timestamptz";
        case PGF_T_DATE:        return "date";
        case PGF_T_JSON:        return "json";
        default:                return "other";
    }
}

/* ---- builders -------------------------------------------------------------- */

static pgf_table_t *find_table(pgf_catalog_t *cat, const char *name) {
    for (int i = 0; i < cat->ntables; i++)
        if (!strcmp(cat->tables[i].name, name)) return &cat->tables[i];
    return NULL;
}

static pgf_table_t *find_or_add_table(pgf_catalog_t *cat, const char *name) {
    pgf_table_t *t = find_table(cat, name);
    if (t) return t;
    /* L-9: grow via a temp so a realloc failure neither leaks the old block nor
     * dereferences NULL / bumps the count. (Boot-time OOM only.) */
    pgf_table_t *grown = realloc(cat->tables, (cat->ntables + 1) * sizeof *cat->tables);
    if (!grown) return NULL;
    cat->tables = grown;
    t = &cat->tables[cat->ntables++];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->pk_index = -1;
    return t;
}

static pgf_column_t *find_column(pgf_table_t *t, const char *name) {
    for (int i = 0; i < t->ncols; i++)
        if (!strcmp(t->cols[i].name, name)) return &t->cols[i];
    return NULL;
}

static pgf_column_t *add_column(pgf_table_t *t, const char *name) {
    pgf_column_t *grown = realloc(t->cols, (t->ncols + 1) * sizeof *t->cols);   /* L-9: see above */
    if (!grown) return NULL;
    t->cols = grown;
    pgf_column_t *c = &t->cols[t->ncols++];
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "%s", name);
    return c;
}

/* ---- introspection --------------------------------------------------------- */

pgf_catalog_t *pgf_catalog_build(void) {
    PGconn *c = db_connection_acquire();
    if (!c) { LOG_ERROR("catalog: no db connection"); return NULL; }

    pgf_catalog_t *cat = calloc(1, sizeof *cat);
    if (!cat) { db_connection_release(c); return NULL; }

    /* 1) columns (ordered so each table's columns are contiguous) */
    PGresult *r = PQexec(c,
        "SELECT c.table_name, c.column_name, c.udt_name, "
        "       c.is_nullable, (c.column_default IS NOT NULL) AS has_default "
        "FROM information_schema.columns c "
        "JOIN information_schema.tables t "
        "  ON t.table_schema=c.table_schema AND t.table_name=c.table_name "
        "WHERE c.table_schema='public' AND t.table_type='BASE TABLE' "
        "  AND c.table_name NOT LIKE 'pgf\\_%' "
        "ORDER BY c.table_name, c.ordinal_position");
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        LOG_ERROR("catalog: columns query failed: %s", PQerrorMessage(c));
        PQclear(r); pgf_catalog_free(cat); db_connection_release(c); return NULL;
    }
    for (int i = 0; i < PQntuples(r); i++) {
        pgf_table_t  *t = find_or_add_table(cat, PQgetvalue(r, i, 0));
        pgf_column_t *col = t ? add_column(t, PQgetvalue(r, i, 1)) : NULL;
        if (!col) {   /* L-9: boot-time OOM growing the catalog — fail the build */
            LOG_ERROR("catalog: out of memory building schema");
            PQclear(r); pgf_catalog_free(cat); db_connection_release(c); return NULL;
        }
        snprintf(col->pg_type, sizeof col->pg_type, "%s", PQgetvalue(r, i, 2));
        col->type        = normalize_type(col->pg_type);
        col->nullable    = strcmp(PQgetvalue(r, i, 3), "YES") == 0;
        col->has_default = strcmp(PQgetvalue(r, i, 4), "t") == 0;
    }
    PQclear(r);

    /* 2) primary keys */
    r = PQexec(c,
        "SELECT tc.table_name, kcu.column_name "
        "FROM information_schema.table_constraints tc "
        "JOIN information_schema.key_column_usage kcu "
        "  ON kcu.constraint_name=tc.constraint_name "
        " AND kcu.table_schema=tc.table_schema "
        "WHERE tc.table_schema='public' AND tc.constraint_type='PRIMARY KEY' "
        "  AND tc.table_name NOT LIKE 'pgf\\_%'");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(r); i++) {
            pgf_table_t *t = find_table(cat, PQgetvalue(r, i, 0));
            if (!t) continue;
            pgf_column_t *col = find_column(t, PQgetvalue(r, i, 1));
            if (!col) continue;
            col->is_pk = true;
            if (t->pk_index < 0) t->pk_index = (int)(col - t->cols);
        }
    }
    PQclear(r);

    /* 3) foreign keys */
    r = PQexec(c,
        "SELECT kcu.table_name, kcu.column_name, "
        "       ccu.table_name AS ftable, ccu.column_name AS fcolumn "
        "FROM information_schema.table_constraints tc "
        "JOIN information_schema.key_column_usage kcu "
        "  ON kcu.constraint_name=tc.constraint_name "
        " AND kcu.table_schema=tc.table_schema "
        "JOIN information_schema.constraint_column_usage ccu "
        "  ON ccu.constraint_name=tc.constraint_name "
        " AND ccu.table_schema=tc.table_schema "
        "WHERE tc.table_schema='public' AND tc.constraint_type='FOREIGN KEY' "
        "  AND tc.table_name NOT LIKE 'pgf\\_%'");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(r); i++) {
            pgf_table_t *t = find_table(cat, PQgetvalue(r, i, 0));
            if (!t) continue;
            pgf_column_t *col = find_column(t, PQgetvalue(r, i, 1));
            if (!col) continue;
            col->is_fk = true;
            snprintf(col->fk_table,  sizeof col->fk_table,  "%s", PQgetvalue(r, i, 2));
            snprintf(col->fk_column, sizeof col->fk_column, "%s", PQgetvalue(r, i, 3));
        }
    }
    PQclear(r);

    db_connection_release(c);
    LOG_INFO("catalog: %d table(s) introspected", cat->ntables);
    return cat;
}

void pgf_catalog_free(pgf_catalog_t *cat) {
    if (!cat) return;
    for (int i = 0; i < cat->ntables; i++) free(cat->tables[i].cols);
    free(cat->tables);
    free(cat);
}

const pgf_table_t *pgf_catalog_find(const pgf_catalog_t *cat, const char *table) {
    if (!cat || !table) return NULL;
    for (int i = 0; i < cat->ntables; i++)
        if (!strcmp(cat->tables[i].name, table)) return &cat->tables[i];
    return NULL;
}

const pgf_column_t *pgf_table_column(const pgf_table_t *t, const char *column) {
    if (!t || !column) return NULL;
    for (int i = 0; i < t->ncols; i++)
        if (!strcmp(t->cols[i].name, column)) return &t->cols[i];
    return NULL;
}

/* ---- JSON ------------------------------------------------------------------ */

cJSON *pgf_catalog_to_cjson(const pgf_catalog_t *cat) {
    cJSON *root   = cJSON_CreateObject();
    cJSON *tables = cJSON_AddArrayToObject(root, "tables");

    for (int i = 0; cat && i < cat->ntables; i++) {
        const pgf_table_t *t = &cat->tables[i];
        cJSON *jt = cJSON_CreateObject();
        cJSON_AddStringToObject(jt, "name", t->name);
        if (t->pk_index >= 0)
            cJSON_AddStringToObject(jt, "primary_key", t->cols[t->pk_index].name);
        else
            cJSON_AddNullToObject(jt, "primary_key");

        cJSON *cols = cJSON_AddArrayToObject(jt, "columns");
        for (int j = 0; j < t->ncols; j++) {
            const pgf_column_t *col = &t->cols[j];
            cJSON *jc = cJSON_CreateObject();
            cJSON_AddStringToObject(jc, "name", col->name);
            cJSON_AddStringToObject(jc, "type", pgf_coltype_name(col->type));
            cJSON_AddStringToObject(jc, "pg_type", col->pg_type);
            cJSON_AddBoolToObject(jc, "nullable", col->nullable);
            cJSON_AddBoolToObject(jc, "primary_key", col->is_pk);
            cJSON_AddBoolToObject(jc, "has_default", col->has_default);
            if (col->is_fk) {
                cJSON *ref = cJSON_AddObjectToObject(jc, "references");
                cJSON_AddStringToObject(ref, "table", col->fk_table);
                cJSON_AddStringToObject(ref, "column", col->fk_column);
            } else {
                cJSON_AddNullToObject(jc, "references");
            }
            cJSON_AddItemToArray(cols, jc);
        }
        cJSON_AddItemToArray(tables, jt);
    }
    return root;
}

char *pgf_catalog_to_json(const pgf_catalog_t *cat) {
    cJSON *root = pgf_catalog_to_cjson(cat);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

/* ---- active catalog -------------------------------------------------------- */

static pgf_catalog_t *g_active = NULL;
void                 pgf_catalog_set_active(pgf_catalog_t *cat) { g_active = cat; }
const pgf_catalog_t *pgf_catalog_active(void)                   { return g_active; }
