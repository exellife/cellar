/* ============================================================================
 * schema_catalog_sqlite_test — per-app catalog via SQLite introspection.
 *
 * libpq-free: builds an in-memory schema and checks the introspected catalog —
 * table/column discovery, declared-type affinity mapping, NOT NULL / default /
 * primary-key / foreign-key flags, and the exclusion of sqlite_* and cel_*
 * internal tables.
 * ============================================================================ */
#include "schema_catalog.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok: %s\n", msg); } \
} while (0)

static const cel_table_t *find(const cel_catalog_t *c, const char *name) {
    for (int i = 0; i < c->ntables; i++)
        if (!strcmp(c->tables[i].name, name)) return &c->tables[i];
    return NULL;
}
static const cel_column_t *col(const cel_table_t *t, const char *name) {
    for (int i = 0; i < t->ncols; i++)
        if (!strcmp(t->cols[i].name, name)) return &t->cols[i];
    return NULL;
}
static void free_catalog(cel_catalog_t *c) {
    if (!c) return;
    for (int i = 0; i < c->ntables; i++) free(c->tables[i].cols);
    free(c->tables); free(c);
}

int main(void) {
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK, "open in-memory db");

    char *err = NULL;
    int rc = sqlite3_exec(db,
        "CREATE TABLE users("
        "  id INTEGER PRIMARY KEY,"
        "  email TEXT NOT NULL,"
        "  active BOOLEAN DEFAULT 1,"
        "  profile JSON,"
        "  score REAL,"
        "  created_at TIMESTAMP);"
        "CREATE TABLE posts("
        "  id INTEGER PRIMARY KEY,"
        "  author_id INTEGER REFERENCES users(id),"
        "  title TEXT NOT NULL);"
        "CREATE TABLE cel_secret(id INTEGER PRIMARY KEY, token TEXT);"  /* must be excluded */
        "CREATE INDEX ix_posts_author ON posts(author_id);",
        NULL, NULL, &err);
    CHECK(rc == SQLITE_OK, "seed schema");
    sqlite3_free(err);

    cel_catalog_t *cat = cel_catalog_build_sqlite(db);
    CHECK(cat != NULL, "build catalog");

    /* exclusions: sqlite_* and cel_* (and indexes) not present; only 2 user tables */
    CHECK(cat->ntables == 2, "two user tables (cel_* + sqlite_* excluded)");
    CHECK(find(cat, "cel_secret") == NULL, "internal cel_ table excluded");

    const cel_table_t *users = find(cat, "users");
    CHECK(users != NULL, "users table found");
    CHECK(users->ncols == 6, "users has 6 columns");

    /* primary key */
    CHECK(users->pk_index >= 0 && !strcmp(users->cols[users->pk_index].name, "id"),
          "pk_index points at id");
    CHECK(col(users, "id")->is_pk, "id flagged primary key");

    /* affinity mapping */
    CHECK(col(users, "id")->type     == CEL_T_BIGINT, "INTEGER -> bigint");
    CHECK(col(users, "email")->type  == CEL_T_TEXT,   "TEXT -> text");
    CHECK(col(users, "active")->type == CEL_T_BOOL,   "BOOLEAN -> bool");
    CHECK(col(users, "profile")->type== CEL_T_JSON,   "JSON -> json");
    CHECK(col(users, "score")->type  == CEL_T_FLOAT,  "REAL -> float");
    CHECK(col(users, "created_at")->type == CEL_T_TIMESTAMPTZ, "TIMESTAMP -> timestamptz");

    /* nullability + defaults */
    CHECK(col(users, "email")->nullable == false, "NOT NULL column not nullable");
    CHECK(col(users, "profile")->nullable == true, "plain column nullable");
    CHECK(col(users, "active")->has_default == true, "DEFAULT column has_default");
    CHECK(col(users, "email")->has_default == false, "no-default column !has_default");

    /* raw declared type preserved */
    CHECK(strcasecmp(col(users, "created_at")->decl_type, "TIMESTAMP") == 0,
          "raw declared type retained");

    /* foreign key */
    const cel_table_t *posts = find(cat, "posts");
    CHECK(posts != NULL, "posts table found");
    const cel_column_t *aid = col(posts, "author_id");
    CHECK(aid && aid->is_fk, "author_id is a foreign key");
    CHECK(aid && strcmp(aid->fk_table, "users") == 0, "FK target table = users");
    CHECK(aid && strcmp(aid->fk_column, "id") == 0, "FK target column = id");

    free_catalog(cat);
    sqlite3_close(db);

    if (failures) { fprintf(stderr, "\n%d check(s) FAILED\n", failures); return 1; }
    fprintf(stderr, "\nall schema_catalog_sqlite checks passed\n");
    return 0;
}
