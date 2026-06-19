/* ============================================================================
 * pgforge — query builder
 *
 * Builds parametrized SELECT statements from a JSON request and the schema
 * catalog. SECURITY MODEL (non-negotiable):
 *   - identifiers (table/column names) are ONLY ever taken from the validated
 *     catalog and quoted; an unknown column is a hard error, never emitted.
 *   - all user-supplied VALUES become $N placeholders bound via PQexecParams;
 *     they are never concatenated into the SQL text.
 * This keeps the generic data layer injection-safe by construction.
 * ============================================================================ */
#ifndef PGF_QUERY_BUILDER_H
#define PGF_QUERY_BUILDER_H

#include "schema_catalog.h"
#include <cjson/cJSON.h>

/* A built statement: SQL text + its positional parameter values (a NULL entry
 * means a SQL NULL bind). Free with pgf_query_free. */
typedef struct {
    char  *sql;
    char **params;     /* nparams entries; each owned (or NULL for SQL NULL) */
    int    nparams;
    int    cap;        /* internal capacity of params */
} pgf_query_t;

/* Default and maximum row counts for DB_LIST when limit is unspecified/too big. */
#define PGF_LIST_DEFAULT_LIMIT 100
#define PGF_LIST_MAX_LIMIT     1000

/* Row-level scoping: a set of AND-ed constraints applied to reads and writes.
 * Reads/writes are restricted to rows matching EVERY rule. `value` is the
 * caller-derived value (e.g. the caller's user/tenant id), ALWAYS bound as a
 * parameter — never concatenated. count == 0 (or a NULL pointer) means no scoping.
 *
 * Each rule is one of three kinds (the list AND-s them together):
 *   EQ  : column = value                         — the base case (owner, tenant)
 *   OR  : (cols[0] = value OR cols[1] = value …)  — caller matches via ANY column
 *   VIA : EXISTS (SELECT 1 FROM via_table         — caller is a related participant
 *                 WHERE via_table.via_ref = <scoped table>.via_local
 *                   AND via_table.via_user = value)
 * EQ rules are also FORCED on INSERT (the new row's column is set to value and any
 * client value for it is ignored). OR and VIA are read/filter-only — they constrain
 * LIST/GET/UPDATE/DELETE but are not forced on CREATE (configure CREATE with an EQ
 * owner column). Identifiers come only from the validated catalog / charset-checked
 * config and are quoted; this keeps the generic layer injection-safe by construction.
 *
 * This generalizes the former single "owner column": owner scoping and tenant
 * scoping stack as rules, and a single role can now be scoped by OR/relationship.
 * Single-tenant is simply the owner-only (or empty) rule set. */
#define PGF_MAX_SCOPE 4
#define PGF_MAX_OR    4
typedef enum {
    PGF_SCOPE_EQ = 0,   /* default (zero) — byte-for-byte unchanged from before */
    PGF_SCOPE_OR,
    PGF_SCOPE_VIA,
} pgf_scope_kind_t;
typedef struct {
    pgf_scope_kind_t kind;          /* 0 == EQ */
    const char *value;              /* caller value, bound as a parameter (all kinds) */
    const char *column;             /* EQ: the scoped column */
    const char *cols[PGF_MAX_OR];   /* OR: columns matched against value */
    int         ncols;              /* OR: number of columns */
    const char *via_table;          /* VIA: the membership/relationship table */
    const char *via_ref;            /* VIA: its column joined to via_local */
    const char *via_local;          /* VIA: the column on the scoped table */
    const char *via_user;           /* VIA: its column matched to value */
} pgf_scope_rule_t;
typedef struct {
    pgf_scope_rule_t rule[PGF_MAX_SCOPE];
    int count;
} pgf_scope_t;

/* Effective keyset sort key (an order column + the PK tiebreaker). */
typedef struct { char column[64]; bool desc; } pgf_sortkey_t;
#define PGF_MAX_SORTKEYS 8

/* Resolve the keyset sort order for `req`: the user's `order` columns plus the PK
 * appended as a tiebreaker (so the order is total). All keys must share one
 * direction (mixed directions are rejected — keyset needs a single direction). On
 * success returns the key count and fills `keys`; on failure returns -1 (errbuf). */
int pgf_resolve_sortkeys(const pgf_table_t *t, const cJSON *req,
                         pgf_sortkey_t *keys, int max, char *errbuf, size_t errlen);

/* Build a LIST query: { select?, where?, order?, limit?, offset? }.
 * `scope` may be NULL. When `cursor` is non-NULL the query is built in KEYSET mode:
 * ORDER BY the resolved sort keys, no OFFSET, and — if `cursor` is a non-empty array
 * of the previous page's key values — a "rows after the cursor" predicate. A NULL
 * `cursor` is classic LIMIT/OFFSET. Return 0 on success; -1 with a reason in errbuf.
 * Build a GET query:  { id } -> single row by primary key. */
int pgf_build_list(const pgf_table_t *t, const cJSON *req, const pgf_scope_t *scope,
                   const cJSON *cursor, pgf_query_t *out, char *errbuf, size_t errlen);
int pgf_build_get(const pgf_table_t *t, const cJSON *req, const pgf_scope_t *scope,
                  pgf_query_t *out, char *errbuf, size_t errlen);

/* Build a COUNT(*) for { where? } over the same filters + scope as the list (no
 * select/order/limit) — the exact total for `count=exact`. `scope` may be NULL. */
int pgf_build_count(const pgf_table_t *t, const cJSON *req, const pgf_scope_t *scope,
                    pgf_query_t *out, char *errbuf, size_t errlen);

/* Build a GROUP BY aggregate from { group?: [col,...], aggregate?: [spec,...], where? }.
 * A spec is "count" (=> count(*)) or "<fn>:<col>" with fn in count/sum/avg/min/max.
 * Selects the group columns + the aggregates, under the same where + scope as the
 * list, GROUP/ORDER BY the group columns. Functions are whitelisted, columns come
 * only from the catalog and are quoted — injection-safe. `scope` may be NULL. At
 * least one of group/aggregate is required. */
int pgf_build_aggregate(const pgf_table_t *t, const cJSON *req, const pgf_scope_t *scope,
                        pgf_query_t *out, char *errbuf, size_t errlen);

/* Write builders, each emitting "... RETURNING <all columns>":
 *   create : { values: {col: v, ...} }       -> INSERT (scoped columns forced)
 *   update : { id, values: {col: v, ...} }    -> UPDATE ... WHERE pk = id [AND scope]
 *   delete : { id }                            -> DELETE ... WHERE pk = id [AND scope]  */
int pgf_build_create(const pgf_table_t *t, const cJSON *req, const pgf_scope_t *scope,
                     pgf_query_t *out, char *errbuf, size_t errlen);
int pgf_build_update(const pgf_table_t *t, const cJSON *req, const pgf_scope_t *scope,
                     pgf_query_t *out, char *errbuf, size_t errlen);
int pgf_build_delete(const pgf_table_t *t, const cJSON *req, const pgf_scope_t *scope,
                     pgf_query_t *out, char *errbuf, size_t errlen);

/* Build an RPC call: SELECT * FROM "fn"(name := $1, ...). `fn` and each arg name
 * (the keys of the `args` object, may be NULL/empty for no args) must be safe
 * identifiers; arg VALUES are bound as parameters. `fn` is quoted; arg names are
 * validated and emitted bare so they match the function's declared parameters. */
int pgf_build_rpc(const char *fn, const cJSON *args,
                  pgf_query_t *out, char *errbuf, size_t errlen);

void pgf_query_free(pgf_query_t *q);

#endif /* PGF_QUERY_BUILDER_H */
