/* ============================================================================
 * cellar — small SQLite query helpers shared by the per-app modules (auth, mfa,
 * …). Uniform text-parameter binding: the engine binds everything as text and
 * lets column affinity coerce (numeric text → integer in an INTEGER column), so
 * one binder covers ids, emails, hashes, timestamps, and counts alike.
 * ============================================================================ */
#ifndef CEL_DB_SQLITE_H
#define CEL_DB_SQLITE_H

#include <stdbool.h>
#include <stddef.h>

struct sqlite3;
struct sqlite3_stmt;

/* Prepare `sql` on `c`, binding n text params (a NULL entry => SQL NULL). On
 * success *out holds the statement (caller finalizes). Returns an SQLite code. */
int cel_db_prep(struct sqlite3 *c, const char *sql, const char *const *p, int n,
                struct sqlite3_stmt **out);

/* Run a statement with text params, expecting no rows back. Returns true if it
 * stepped to completion (DONE/ROW) without error. */
bool cel_db_exec(struct sqlite3 *c, const char *sql, const char *const *p, int n);

/* Run a query expected to yield at most one row; copy column 0 (as text) into
 * `out`. Returns 1 if a row was found, 0 if none, -1 on a query error. */
int cel_db_one_text(struct sqlite3 *c, const char *sql, const char *const *p, int n,
                    char *out, size_t out_size);

/* Current unix-epoch seconds — for computing expiries to bind (SQLite has no now()). */
long cel_now_epoch(void);

#endif /* CEL_DB_SQLITE_H */
