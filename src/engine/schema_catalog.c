#include "schema_catalog.h"

#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>

/* The catalog is now built per-app by SQLite introspection (schema_catalog_sqlite.c,
 * cel_catalog_build_sqlite). This file holds the shared, storage-agnostic pieces:
 * the cel_catalog_t accessors, JSON serialization, and the active-catalog pointer. */

/* ---- type name ------------------------------------------------------------- */

const char *cel_coltype_name(cel_coltype_t t) {
    switch (t) {
        case CEL_T_INT:         return "int";
        case CEL_T_BIGINT:      return "bigint";
        case CEL_T_FLOAT:       return "float";
        case CEL_T_NUMERIC:     return "numeric";
        case CEL_T_BOOL:        return "bool";
        case CEL_T_TEXT:        return "text";
        case CEL_T_UUID:        return "uuid";
        case CEL_T_TIMESTAMPTZ: return "timestamptz";
        case CEL_T_DATE:        return "date";
        case CEL_T_JSON:        return "json";
        default:                return "other";
    }
}

/* ---- lifecycle / lookup ---------------------------------------------------- */

void cel_catalog_free(cel_catalog_t *cat) {
    if (!cat) return;
    for (int i = 0; i < cat->ntables; i++) free(cat->tables[i].cols);
    free(cat->tables);
    free(cat);
}

const cel_table_t *cel_catalog_find(const cel_catalog_t *cat, const char *table) {
    if (!cat || !table) return NULL;
    for (int i = 0; i < cat->ntables; i++)
        if (!strcmp(cat->tables[i].name, table)) return &cat->tables[i];
    return NULL;
}

const cel_column_t *cel_table_column(const cel_table_t *t, const char *column) {
    if (!t || !column) return NULL;
    for (int i = 0; i < t->ncols; i++)
        if (!strcmp(t->cols[i].name, column)) return &t->cols[i];
    return NULL;
}

/* ---- JSON ------------------------------------------------------------------ */

cJSON *cel_catalog_to_cjson(const cel_catalog_t *cat) {
    cJSON *root   = cJSON_CreateObject();
    cJSON *tables = cJSON_AddArrayToObject(root, "tables");

    for (int i = 0; cat && i < cat->ntables; i++) {
        const cel_table_t *t = &cat->tables[i];
        cJSON *jt = cJSON_CreateObject();
        cJSON_AddStringToObject(jt, "name", t->name);
        if (t->pk_index >= 0)
            cJSON_AddStringToObject(jt, "primary_key", t->cols[t->pk_index].name);
        else
            cJSON_AddNullToObject(jt, "primary_key");

        cJSON *cols = cJSON_AddArrayToObject(jt, "columns");
        for (int j = 0; j < t->ncols; j++) {
            const cel_column_t *col = &t->cols[j];
            cJSON *jc = cJSON_CreateObject();
            cJSON_AddStringToObject(jc, "name", col->name);
            cJSON_AddStringToObject(jc, "type", cel_coltype_name(col->type));
            cJSON_AddStringToObject(jc, "pg_type", col->pg_type);   /* raw declared type */
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

char *cel_catalog_to_json(const cel_catalog_t *cat) {
    cJSON *root = cel_catalog_to_cjson(cat);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

/* ---- active catalog -------------------------------------------------------- */

static cel_catalog_t *g_active = NULL;
void                 cel_catalog_set_active(cel_catalog_t *cat) { g_active = cat; }
const cel_catalog_t *cel_catalog_active(void)                   { return g_active; }
