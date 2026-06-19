/* ============================================================================
 * query_exec_sqlite_test — end-to-end proof that the query builder emits valid,
 * executable SQLite. Ties together Phase-1 steps 2-4:
 *
 *   query_builder (?N + RETURNING)  →  bind+step on a real sqlite3
 *      →  result_json (typed rows)  ←  schema_catalog_sqlite (the catalog)
 *
 * The run() helper here is the prototype of the step-5 executor (api.c): prepare
 * the built SQL, bind params[i] to ?(i+1) as text or NULL, step, serialize.
 * libpq-free: in-memory db, local cel_table_column (shared by builder+serializer).
 * ============================================================================ */
#include "query_builder.h"
#include "schema_catalog.h"
#include "result_json.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the one symbol query_builder.c and result_json.c both need (libpq-free) */
const cel_column_t *cel_table_column(const cel_table_t *t, const char *column) {
    for (int i = 0; i < t->ncols; i++)
        if (!strcmp(t->cols[i].name, column)) return &t->cols[i];
    return NULL;
}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok: %s\n", msg); } \
} while (0)

/* prototype of the step-5 executor: prepare -> bind ?N -> step -> serialize */
static cJSON *run(sqlite3 *db, cel_query_t *q, const cel_table_t *t) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, q->sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "  prepare failed: %s\n  SQL: %s\n", sqlite3_errmsg(db), q->sql);
        return NULL;
    }
    for (int i = 0; i < q->nparams; i++) {
        if (q->params[i]) sqlite3_bind_text(st, i + 1, q->params[i], -1, SQLITE_TRANSIENT);
        else              sqlite3_bind_null(st, i + 1);
    }
    cJSON *rows = t ? cel_stmt_rows_to_json(st, t) : cel_stmt_result_to_json(st);
    sqlite3_finalize(st);
    return rows;
}

int main(void) {
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK, "open in-memory db");
    sqlite3_exec(db,
        "CREATE TABLE notes("
        "  id INTEGER PRIMARY KEY,"
        "  owner_id TEXT,"
        "  title TEXT,"
        "  qty INTEGER,"
        "  active BOOLEAN);", NULL, NULL, NULL);

    cel_catalog_t *cat = cel_catalog_build_sqlite(db);
    const cel_table_t *t = NULL;
    for (int i = 0; cat && i < cat->ntables; i++)
        if (!strcmp(cat->tables[i].name, "notes")) t = &cat->tables[i];
    CHECK(t != NULL, "catalog has notes");

    /* owner scope: every write forces owner_id = "u1", every read filters by it */
    cel_scope_t owner = { .count = 1 };
    owner.rule[0].column = "owner_id";
    owner.rule[0].value  = "u1";

    char err[256];
    long new_id = -1;

    /* CREATE — INSERT ... RETURNING; bool true binds as 1, comes back as JSON bool */
    {
        cJSON *req = cJSON_CreateObject();
        cJSON *vals = cJSON_AddObjectToObject(req, "values");
        cJSON_AddStringToObject(vals, "title", "hi");
        cJSON_AddNumberToObject(vals, "qty", 3);
        cJSON_AddBoolToObject(vals, "active", 1);
        cel_query_t q;
        CHECK(cel_build_create(t, req, &owner, &q, err, sizeof err) == 0, "build create");
        cJSON *rows = run(db, &q, t);
        CHECK(rows && cJSON_GetArraySize(rows) == 1, "create RETURNING one row");
        cJSON *r = cJSON_GetArrayItem(rows, 0);
        CHECK(cJSON_IsNumber(cJSON_GetObjectItem(r, "id")), "returned id is a number");
        CHECK(!strcmp(cJSON_GetObjectItem(r, "owner_id")->valuestring, "u1"),
              "owner forced to caller (u1)");
        CHECK(cJSON_GetObjectItem(r, "qty")->valuedouble == 3, "qty round-trips as number");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r, "active")),
              "bool true round-trips (bound 1 -> BOOLEAN -> true)");
        new_id = (long)cJSON_GetObjectItem(r, "id")->valuedouble;
        cJSON_Delete(rows); cel_query_free(&q); cJSON_Delete(req);
    }

    /* LIST — SELECT ... WHERE owner_id = ?1 */
    {
        cJSON *req = cJSON_CreateObject();
        cel_query_t q;
        CHECK(cel_build_list(t, req, &owner, NULL, &q, err, sizeof err) == 0, "build list");
        cJSON *rows = run(db, &q, t);
        CHECK(rows && cJSON_GetArraySize(rows) == 1, "list returns the owned row");
        cJSON_Delete(rows); cel_query_free(&q); cJSON_Delete(req);
    }

    /* UPDATE — UPDATE ... WHERE pk = ?N AND owner = ?N RETURNING */
    {
        cJSON *req = cJSON_CreateObject();
        cJSON_AddNumberToObject(req, "id", new_id);
        cJSON_AddStringToObject(cJSON_AddObjectToObject(req, "values"), "title", "bye");
        cel_query_t q;
        CHECK(cel_build_update(t, req, &owner, &q, err, sizeof err) == 0, "build update");
        cJSON *rows = run(db, &q, t);
        CHECK(rows && cJSON_GetArraySize(rows) == 1, "update RETURNING one row");
        CHECK(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(rows, 0), "title")->valuestring, "bye"),
              "title updated");
        cJSON_Delete(rows); cel_query_free(&q); cJSON_Delete(req);
    }

    /* LIKE filter — SQLite LIKE, case-insensitive */
    {
        cJSON *req = cJSON_CreateObject();
        cJSON *w = cJSON_AddObjectToObject(req, "where");
        cJSON_AddStringToObject(cJSON_AddObjectToObject(w, "title"), "like", "BY%");  /* matches "bye" */
        cel_query_t q;
        CHECK(cel_build_list(t, req, &owner, NULL, &q, err, sizeof err) == 0, "build list+like");
        cJSON *rows = run(db, &q, t);
        CHECK(rows && cJSON_GetArraySize(rows) == 1, "LIKE matches case-insensitively");
        cJSON_Delete(rows); cel_query_free(&q); cJSON_Delete(req);
    }

    /* DELETE — DELETE ... RETURNING, then the table is empty for this owner */
    {
        cJSON *req = cJSON_CreateObject();
        cJSON_AddNumberToObject(req, "id", new_id);
        cel_query_t q;
        CHECK(cel_build_delete(t, req, &owner, &q, err, sizeof err) == 0, "build delete");
        cJSON *rows = run(db, &q, t);
        CHECK(rows && cJSON_GetArraySize(rows) == 1, "delete RETURNING the removed row");
        cJSON_Delete(rows); cel_query_free(&q); cJSON_Delete(req);

        cJSON *req2 = cJSON_CreateObject();
        cel_query_t q2;
        cel_build_list(t, req2, &owner, NULL, &q2, err, sizeof err);
        cJSON *rows2 = run(db, &q2, t);
        CHECK(rows2 && cJSON_GetArraySize(rows2) == 0, "row is gone after delete");
        cJSON_Delete(rows2); cel_query_free(&q2); cJSON_Delete(req2);
    }

    /* free catalog (local; the public cel_catalog_free lives in the PG TU) */
    for (int i = 0; cat && i < cat->ntables; i++) free(cat->tables[i].cols);
    free(cat->tables); free(cat);
    sqlite3_close(db);

    if (failures) { fprintf(stderr, "\n%d check(s) FAILED\n", failures); return 1; }
    fprintf(stderr, "\nall query_exec_sqlite checks passed\n");
    return 0;
}
