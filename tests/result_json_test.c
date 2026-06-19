/* ============================================================================
 * result_json_test — SQLite result → typed JSON serializer.
 *
 * libpq-free: uses an in-memory SQLite database and a hand-built catalog table.
 * Provides its own cel_table_column (the pure lookup) so the test links neither
 * schema_catalog.c nor Postgres.
 * ============================================================================ */
#include "result_json.h"

#include <stdio.h>
#include <string.h>

/* --- the one catalog helper result_json.c calls, defined here (libpq-free) --- */
const cel_column_t *cel_table_column(const cel_table_t *t, const char *column) {
    for (int i = 0; i < t->ncols; i++)
        if (strcmp(t->cols[i].name, column) == 0) return &t->cols[i];
    return NULL;
}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok: %s\n", msg); } \
} while (0)

static sqlite3 *open_mem(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) { fprintf(stderr, "open :memory: failed\n"); }
    return db;
}

static void col(cel_column_t *c, const char *name, cel_coltype_t type) {
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "%s", name);
    c->type = type;
}

int main(void) {
    sqlite3 *db = open_mem();
    CHECK(db != NULL, "open in-memory db");

    char *err = NULL;
    int rc = sqlite3_exec(db,
        "CREATE TABLE t(id INTEGER PRIMARY KEY, qty INTEGER, price REAL, "
        "               active INTEGER, name TEXT, meta TEXT);"
        "INSERT INTO t VALUES(1, 7, 9.5, 1, 'widget', '{\"a\":42}');"
        "INSERT INTO t VALUES(2, NULL, 0.0, 0, 'gadget', NULL);",
        NULL, NULL, &err);
    CHECK(rc == SQLITE_OK, "seed schema + rows");
    sqlite3_free(err);

    /* catalog: declare the engine-visible types (SQLite stored bools as 0/1, json as text) */
    cel_column_t cols[6];
    col(&cols[0], "id",     CEL_T_INT);
    col(&cols[1], "qty",    CEL_T_INT);
    col(&cols[2], "price",  CEL_T_FLOAT);
    col(&cols[3], "active", CEL_T_BOOL);
    col(&cols[4], "name",   CEL_T_TEXT);
    col(&cols[5], "meta",   CEL_T_JSON);
    cel_table_t tbl = { .ncols = 6, .cols = cols, .pk_index = 0 };
    snprintf(tbl.name, sizeof tbl.name, "%s", "t");

    /* --- catalog-typed path --- */
    {
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(db, "SELECT id,qty,price,active,name,meta FROM t ORDER BY id", -1, &st, NULL);
        cJSON *arr = cel_stmt_rows_to_json(st, &tbl);
        sqlite3_finalize(st);
        CHECK(cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 2, "two rows serialized");

        cJSON *r0 = cJSON_GetArrayItem(arr, 0);
        cJSON *r1 = cJSON_GetArrayItem(arr, 1);

        cJSON *qty0 = cJSON_GetObjectItem(r0, "qty");
        CHECK(cJSON_IsNumber(qty0) && qty0->valuedouble == 7, "int -> JSON number");

        cJSON *price0 = cJSON_GetObjectItem(r0, "price");
        CHECK(cJSON_IsNumber(price0) && price0->valuedouble == 9.5, "real -> JSON number");

        cJSON *act0 = cJSON_GetObjectItem(r0, "active");
        cJSON *act1 = cJSON_GetObjectItem(r1, "active");
        CHECK(cJSON_IsTrue(act0), "declared bool 1 -> true");
        CHECK(cJSON_IsFalse(act1), "declared bool 0 -> false");

        cJSON *name0 = cJSON_GetObjectItem(r0, "name");
        CHECK(cJSON_IsString(name0) && strcmp(name0->valuestring, "widget") == 0, "text -> string");

        cJSON *meta0 = cJSON_GetObjectItem(r0, "meta");
        CHECK(cJSON_IsObject(meta0), "json column parsed to object");
        cJSON *a = cJSON_GetObjectItem(meta0, "a");
        CHECK(cJSON_IsNumber(a) && a->valuedouble == 42, "nested json value intact");

        cJSON *qty1 = cJSON_GetObjectItem(r1, "qty");
        cJSON *meta1 = cJSON_GetObjectItem(r1, "meta");
        CHECK(cJSON_IsNull(qty1), "SQL NULL int -> JSON null");
        CHECK(cJSON_IsNull(meta1), "SQL NULL json -> JSON null");

        cJSON_Delete(arr);
    }

    /* --- runtime-typed (table-less) path --- */
    {
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(db, "SELECT COUNT(*) AS n, 1.5 AS f, 'hi' AS s, NULL AS z", -1, &st, NULL);
        cJSON *arr = cel_stmt_result_to_json(st);
        sqlite3_finalize(st);
        CHECK(cJSON_GetArraySize(arr) == 1, "one aggregate row");
        cJSON *r = cJSON_GetArrayItem(arr, 0);
        CHECK(cJSON_IsNumber(cJSON_GetObjectItem(r, "n")), "runtime INTEGER -> number");
        CHECK(cJSON_IsNumber(cJSON_GetObjectItem(r, "f")), "runtime FLOAT -> number");
        CHECK(cJSON_IsString(cJSON_GetObjectItem(r, "s")), "runtime TEXT -> string");
        CHECK(cJSON_IsNull(cJSON_GetObjectItem(r, "z")),   "runtime NULL -> null");
        cJSON_Delete(arr);
    }

    sqlite3_close(db);
    if (failures) { fprintf(stderr, "\n%d check(s) FAILED\n", failures); return 1; }
    fprintf(stderr, "\nall result_json checks passed\n");
    return 0;
}
