#include "query_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tiny string builder --------------------------------------------------- */

typedef struct { char *buf; size_t len, cap; } sb_t;

static int sb_init(sb_t *s) {
    s->cap = 256; s->len = 0;
    s->buf = malloc(s->cap);
    if (!s->buf) return -1;
    s->buf[0] = '\0';
    return 0;
}
static int sb_putn(sb_t *s, const char *p, size_t n) {
    if (s->len + n + 1 > s->cap) {
        size_t nc = s->cap * 2;
        while (nc < s->len + n + 1) nc *= 2;
        char *nb = realloc(s->buf, nc);
        if (!nb) return -1;
        s->buf = nb; s->cap = nc;
    }
    memcpy(s->buf + s->len, p, n);
    s->len += n; s->buf[s->len] = '\0';
    return 0;
}
static int sb_puts(sb_t *s, const char *p) { return sb_putn(s, p, strlen(p)); }

/* A charset-restricted identifier ([A-Za-z_][A-Za-z0-9_]*): used for names that
 * are NOT catalog-validated (RPC function + argument names), so they are checked
 * before being emitted into SQL. */
static int is_safe_ident(const char *s) {
    if (!s || !*s) return 0;
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_')) return 0;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return 0;
    return 1;
}

/* Quote a SQL identifier (catalog-validated, so just defensively escape "). */
static int sb_put_ident(sb_t *s, const char *ident) {
    if (sb_putn(s, "\"", 1)) return -1;
    for (const char *p = ident; *p; p++) {
        if (*p == '"' && sb_putn(s, "\"", 1)) return -1;
        if (sb_putn(s, p, 1)) return -1;
    }
    return sb_putn(s, "\"", 1);
}

/* ---- query param management ------------------------------------------------ */

static int q_init(cel_query_t *q) {
    memset(q, 0, sizeof *q);
    q->cap = 8;
    q->params = malloc(q->cap * sizeof *q->params);
    return q->params ? 0 : -1;
}
/* Append a parameter value (owned string, or NULL for SQL NULL). */
static int q_push(cel_query_t *q, char *owned_or_null) {
    if (q->nparams == q->cap) {
        int nc = q->cap * 2;
        char **np = realloc(q->params, nc * sizeof *np);
        if (!np) return -1;
        q->params = np; q->cap = nc;
    }
    q->params[q->nparams++] = owned_or_null;
    return 0;
}
/* Emit "?N" for the next parameter slot into the SQL buffer. SQLite numbered
 * placeholders (?1, ?2, …); params are pushed in the same order, no reuse, so
 * the executor binds params[i] to index i+1. */
static int q_placeholder(cel_query_t *q, sb_t *sql) {
    char ph[16];
    snprintf(ph, sizeof ph, "?%d", q->nparams + 1);
    return sb_puts(sql, ph);
}

void cel_query_free(cel_query_t *q) {
    if (!q) return;
    for (int i = 0; i < q->nparams; i++) free(q->params[i]);
    free(q->params);
    free(q->sql);
    memset(q, 0, sizeof *q);
}

/* ---- value coercion -------------------------------------------------------- */

/* Convert a JSON scalar to the text bound as a SQLite parameter (malloc'd).
 * SQLite has no boolean type, so booleans bind as "1"/"0" — a column with
 * INTEGER/NUMERIC affinity then stores them numerically (binding "true"/"false"
 * text would NOT coerce). Returns NULL for unsupported kinds. Caller must
 * distinguish "JSON null" beforehand. */
static char *json_scalar_text(const cJSON *v) {
    if (cJSON_IsBool(v)) return strdup(cJSON_IsTrue(v) ? "1" : "0");
    if (cJSON_IsString(v)) return strdup(v->valuestring);
    if (cJSON_IsNumber(v)) {
        char b[40];
        double d = v->valuedouble;
        if (d == (double)(long long)d) snprintf(b, sizeof b, "%lld", (long long)d);
        else                           snprintf(b, sizeof b, "%.17g", d);
        return strdup(b);
    }
    /* objects/arrays serialize to JSON text — stored as TEXT (SQLite has no jsonb;
     * the serializer parses JSON-typed columns back on read) */
    if (cJSON_IsObject(v) || cJSON_IsArray(v)) return cJSON_PrintUnformatted(v);
    return NULL;
}

/* ---- WHERE operators ------------------------------------------------------- */

static const char *sql_operator(const char *op) {
    if (!strcmp(op, "eq"))    return "=";
    if (!strcmp(op, "neq"))   return "<>";
    if (!strcmp(op, "lt"))    return "<";
    if (!strcmp(op, "lte"))   return "<=";
    if (!strcmp(op, "gt"))    return ">";
    if (!strcmp(op, "gte"))   return ">=";
    if (!strcmp(op, "like"))  return "LIKE";
    /* SQLite has no ILIKE; its LIKE is already case-insensitive for ASCII, so
     * "ilike" maps to LIKE (a deliberate Postgres→SQLite behavior change). */
    if (!strcmp(op, "ilike")) return "LIKE";
    return NULL; /* "in" handled separately; anything else is invalid */
}

#define FAIL(...) do { snprintf(errbuf, errlen, __VA_ARGS__); return -1; } while (0)

/* Append one "col OP value" condition (or IS NULL / IN (...) forms). */
static int build_condition(const cel_table_t *t, cel_query_t *q, sb_t *sql,
                           const char *col, const char *op, const cJSON *val,
                           char *errbuf, size_t errlen) {
    if (sb_put_ident(sql, col)) return -1;

    if (!strcmp(op, "in")) {
        if (!cJSON_IsArray(val)) FAIL("operator 'in' on '%s' needs an array", col);
        if (sb_puts(sql, " IN (")) return -1;
        int n = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, val) {
            if (n++ && sb_puts(sql, ", ")) return -1;
            if (cJSON_IsNull(e)) FAIL("'in' on '%s' cannot contain null", col);
            char *txt = json_scalar_text(e);
            if (!txt) FAIL("'in' on '%s' has an unsupported value", col);
            if (q_placeholder(q, sql) || q_push(q, txt)) { free(txt); return -1; }
        }
        if (n == 0) FAIL("'in' on '%s' needs a non-empty array", col);
        return sb_puts(sql, ")");
    }

    if (!strcmp(op, "between")) {
        if (!cJSON_IsArray(val) || cJSON_GetArraySize(val) != 2)
            FAIL("'between' on '%s' needs a [low, high] array", col);
        char *lo = json_scalar_text(cJSON_GetArrayItem(val, 0));
        if (!lo) FAIL("'between' on '%s' has an unsupported low value", col);
        char *hi = json_scalar_text(cJSON_GetArrayItem(val, 1));
        if (!hi) { free(lo); FAIL("'between' on '%s' has an unsupported high value", col); }
        if (sb_puts(sql, " BETWEEN ") || q_placeholder(q, sql) || q_push(q, lo)) { free(lo); free(hi); return -1; }
        if (sb_puts(sql, " AND ") || q_placeholder(q, sql) || q_push(q, hi)) { free(hi); return -1; }
        return 0;
    }

    /* null comparisons map to IS [NOT] NULL */
    if (cJSON_IsNull(val)) {
        if (!strcmp(op, "eq"))  return sb_puts(sql, " IS NULL");
        if (!strcmp(op, "neq")) return sb_puts(sql, " IS NOT NULL");
        FAIL("operator '%s' on '%s' cannot take null", op, col);
    }

    const char *sqlop = sql_operator(op);
    if (!sqlop) FAIL("unknown operator '%s' on '%s'", op, col);

    char *txt = json_scalar_text(val);
    if (!txt) FAIL("unsupported value for '%s'", col);

    if (sb_puts(sql, " ") || sb_puts(sql, sqlop) || sb_puts(sql, " ") ||
        q_placeholder(q, sql) || q_push(q, txt)) { free(txt); return -1; }
    return 0;
}

/* ---- SELECT list ----------------------------------------------------------- */

static int build_select_list(const cel_table_t *t, const cJSON *req, sb_t *sql,
                             char *errbuf, size_t errlen) {
    const cJSON *sel = req ? cJSON_GetObjectItemCaseSensitive(req, "select") : NULL;
    if (sel && cJSON_IsArray(sel) && cJSON_GetArraySize(sel) > 0) {
        int n = 0;
        const cJSON *c;
        cJSON_ArrayForEach(c, sel) {
            if (!cJSON_IsString(c)) FAIL("select entries must be column names");
            if (!cel_table_column(t, c->valuestring))
                FAIL("unknown column '%s' in select", c->valuestring);
            if (n++ && sb_puts(sql, ", ")) return -1;
            if (sb_put_ident(sql, c->valuestring)) return -1;
        }
        return 0;
    }
    /* default: every catalog column, explicitly (deterministic output) */
    for (int i = 0; i < t->ncols; i++) {
        if (i && sb_puts(sql, ", ")) return -1;
        if (sb_put_ident(sql, t->cols[i].name)) return -1;
    }
    return 0;
}

/* ---- WHERE / ORDER / LIMIT ------------------------------------------------- */

/* Emit "?N" bound to a literal (owned) value. */
static int append_value_param(cel_query_t *q, sb_t *sql, const char *value) {
    char *v = strdup(value);
    if (!v) return -1;
    if (q_placeholder(q, sql) || q_push(q, v)) { free(v); return -1; }
    return 0;
}

/* Append "col = ?N" bound to a literal value (used for ownership predicates). */
static int append_eq_param(cel_query_t *q, sb_t *sql, const char *col, const char *value) {
    if (sb_put_ident(sql, col) || sb_puts(sql, " = ")) return -1;
    return append_value_param(q, sql, value);
}

/* Number of active scope rules (0 for a NULL scope). */
static int scope_count(const cel_scope_t *s) { return s ? s->count : 0; }

/* Emit "tbl"."col" (a qualified identifier) for VIA join predicates. */
static int append_qualified(sb_t *sql, const char *tbl, const char *col) {
    return sb_put_ident(sql, tbl) || sb_puts(sql, ".") || sb_put_ident(sql, col);
}

/* Append one scope rule as a parametrized predicate, per its kind. `t` is the
 * table being scoped (needed to qualify the VIA correlation). EQ output is
 * identical to the original `col = ?N` form (pinned by the regression test). */
static int append_scope_predicate(const cel_table_t *t, cel_query_t *q, sb_t *sql,
                                  const cel_scope_rule_t *r) {
    if (r->kind == CEL_SCOPE_OR) {
        if (sb_puts(sql, "(")) return -1;
        for (int i = 0; i < r->ncols; i++) {
            if (i && sb_puts(sql, " OR ")) return -1;
            if (append_eq_param(q, sql, r->cols[i], r->value)) return -1;
        }
        return sb_puts(sql, ")");
    }
    if (r->kind == CEL_SCOPE_VIA) {
        if (sb_puts(sql, "EXISTS (SELECT 1 FROM ") || sb_put_ident(sql, r->via_table) ||
            sb_puts(sql, " WHERE ") ||
            append_qualified(sql, r->via_table, r->via_ref) || sb_puts(sql, " = ") ||
            append_qualified(sql, t->name, r->via_local) || sb_puts(sql, " AND ") ||
            append_qualified(sql, r->via_table, r->via_user) || sb_puts(sql, " = "))
            return -1;
        if (append_value_param(q, sql, r->value)) return -1;
        return sb_puts(sql, ")");
    }
    return append_eq_param(q, sql, r->column, r->value);   /* EQ */
}

/* Build a where node: the AND of its keys. A key is a logical operator (and/or/
 * not — recursed, parenthesized) or a column name (its operator conditions, AND-ed).
 * The flat all-column form is byte-for-byte unchanged from before. */
static int build_node(const cel_table_t *t, cel_query_t *q, sb_t *sql,
                      const cJSON *node, char *errbuf, size_t errlen) {
    int n = 0;
    const cJSON *child;
    cJSON_ArrayForEach(child, node) {
        const char *key = child->string;
        if (!strcmp(key, "and") || !strcmp(key, "or")) {
            if (!cJSON_IsArray(child) || cJSON_GetArraySize(child) == 0)
                FAIL("'%s' needs a non-empty array", key);
            if (n++ && sb_puts(sql, " AND ")) return -1;
            const char *join = key[0] == 'a' ? " AND " : " OR ";
            if (sb_puts(sql, "(")) return -1;
            const cJSON *el; int m = 0;
            cJSON_ArrayForEach(el, child) {
                if (!cJSON_IsObject(el)) FAIL("'%s' entries must be objects", key);
                if (m++ && sb_puts(sql, join)) return -1;
                if (build_node(t, q, sql, el, errbuf, errlen)) return -1;
            }
            if (sb_puts(sql, ")")) return -1;
        } else if (!strcmp(key, "not")) {
            if (!cJSON_IsObject(child)) FAIL("'not' must be an object");
            if (n++ && sb_puts(sql, " AND ")) return -1;
            if (sb_puts(sql, "NOT (")) return -1;
            if (build_node(t, q, sql, child, errbuf, errlen)) return -1;
            if (sb_puts(sql, ")")) return -1;
        } else {
            const char *col = key;
            if (!cel_table_column(t, col)) FAIL("unknown column '%s' in where", col);
            if (!cJSON_IsObject(child)) FAIL("'where.%s' must be an object like {\"eq\": v}", col);
            if (cJSON_GetArraySize(child) == 0) FAIL("'where.%s' needs at least one operator", col);
            const cJSON *opspec;
            cJSON_ArrayForEach(opspec, child) {
                if (n++ && sb_puts(sql, " AND ")) return -1;
                if (build_condition(t, q, sql, col, opspec->string, opspec, errbuf, errlen))
                    return -1;
            }
        }
    }
    return 0;
}

static int build_where(const cel_table_t *t, const cJSON *req, const cel_scope_t *scope,
                       cel_query_t *q, sb_t *sql, char *errbuf, size_t errlen) {
    const cJSON *where = req ? cJSON_GetObjectItemCaseSensitive(req, "where") : NULL;
    if (where && !cJSON_IsObject(where)) FAIL("'where' must be an object");
    bool have_where = cJSON_IsObject(where) && cJSON_GetArraySize(where) > 0;
    int nscope = scope_count(scope);
    if (!have_where && nscope == 0) return 0;

    if (sb_puts(sql, " WHERE ")) return -1;
    int n = 0;
    if (have_where) {
        if (build_node(t, q, sql, where, errbuf, errlen)) return -1;
        n = 1;
    }
    for (int i = 0; i < nscope; i++) {
        if (n++ && sb_puts(sql, " AND ")) return -1;
        if (append_scope_predicate(t, q, sql, &scope->rule[i])) return -1;
    }
    return 0;
}

static int build_order(const cel_table_t *t, const cJSON *req, sb_t *sql,
                       char *errbuf, size_t errlen) {
    const cJSON *order = req ? cJSON_GetObjectItemCaseSensitive(req, "order") : NULL;
    if (!order) return 0;
    if (!cJSON_IsArray(order)) FAIL("'order' must be an array");
    if (cJSON_GetArraySize(order) == 0) return 0;

    if (sb_puts(sql, " ORDER BY ")) return -1;
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, order) {
        if (!cJSON_IsString(e)) FAIL("'order' entries must be strings");
        const char *spec = e->valuestring;
        int desc = (spec[0] == '-');
        const char *col = desc ? spec + 1 : spec;
        if (!cel_table_column(t, col)) FAIL("unknown column '%s' in order", col);
        if (n++ && sb_puts(sql, ", ")) return -1;
        if (sb_put_ident(sql, col)) return -1;
        if (sb_puts(sql, desc ? " DESC" : " ASC")) return -1;
    }
    return 0;
}

/* limit/offset are integers we sanitize and inline (never user text). */
static int build_limit_offset(const cJSON *req, sb_t *sql,
                              char *errbuf, size_t errlen) {
    long limit = CEL_LIST_DEFAULT_LIMIT, offset = 0;
    const cJSON *jl = req ? cJSON_GetObjectItemCaseSensitive(req, "limit") : NULL;
    const cJSON *jo = req ? cJSON_GetObjectItemCaseSensitive(req, "offset") : NULL;
    if (jl) {
        if (!cJSON_IsNumber(jl)) FAIL("'limit' must be a number");
        limit = (long)jl->valuedouble;
        if (limit < 1) limit = 1;
        if (limit > CEL_LIST_MAX_LIMIT) limit = CEL_LIST_MAX_LIMIT;
    }
    if (jo) {
        if (!cJSON_IsNumber(jo)) FAIL("'offset' must be a number");
        offset = (long)jo->valuedouble;
        if (offset < 0) offset = 0;
    }
    char clause[64];
    snprintf(clause, sizeof clause, " LIMIT %ld OFFSET %ld", limit, offset);
    return sb_puts(sql, clause);
}

/* ---- keyset pagination ----------------------------------------------------- */

int cel_resolve_sortkeys(const cel_table_t *t, const cJSON *req,
                         cel_sortkey_t *keys, int max, char *errbuf, size_t errlen) {
    int n = 0;
    bool have = false, dir = false;
    const cJSON *order = req ? cJSON_GetObjectItemCaseSensitive(req, "order") : NULL;
    if (order) {
        if (!cJSON_IsArray(order)) FAIL("'order' must be an array");
        const cJSON *e;
        cJSON_ArrayForEach(e, order) {
            if (!cJSON_IsString(e)) FAIL("'order' entries must be strings");
            const char *spec = e->valuestring;
            bool desc = (spec[0] == '-');
            const char *col = desc ? spec + 1 : spec;
            if (!cel_table_column(t, col)) FAIL("unknown column '%s' in order", col);
            if (have && desc != dir) FAIL("keyset pagination needs all order columns in one direction");
            if (n >= max) FAIL("too many sort columns for keyset");
            snprintf(keys[n].column, sizeof keys[n].column, "%s", col);
            keys[n].desc = desc; n++;
            dir = desc; have = true;
        }
    }
    if (t->pk_index < 0) FAIL("keyset pagination requires a primary key");
    const char *pk = t->cols[t->pk_index].name;
    for (int i = 0; i < n; i++) if (!strcmp(keys[i].column, pk)) return n;  /* PK already in order */
    if (n >= max) FAIL("too many sort columns for keyset");
    snprintf(keys[n].column, sizeof keys[n].column, "%s", pk);
    keys[n].desc = have ? dir : false;
    return n + 1;
}

/* Append the "rows strictly after the cursor" predicate for `keys` (all one
 * direction). Lexicographic, expanded so each comparison is `col OP ?n` (binary,
 * so the bound value's type is inferred from the column). `prefix` is already
 * emitted by the caller. */
static int build_keyset_predicate(cel_query_t *q, sb_t *sql, const cel_sortkey_t *keys,
                                  int nk, const cJSON *cv, char *errbuf, size_t errlen) {
    if (!cJSON_IsArray(cv) || cJSON_GetArraySize(cv) != nk) FAIL("cursor does not match the sort order");
    const char *op = keys[0].desc ? "<" : ">";
    if (sb_puts(sql, "(")) return -1;
    for (int i = 0; i < nk; i++) {
        if (i && sb_puts(sql, " OR ")) return -1;
        if (sb_puts(sql, "(")) return -1;
        for (int j = 0; j <= i; j++) {
            char *txt = json_scalar_text(cJSON_GetArrayItem(cv, j));
            if (!txt) FAIL("invalid cursor value");
            const char *cmp = (j < i) ? " = " : (op[0] == '<' ? " < " : " > ");
            if (j && sb_puts(sql, " AND ")) { free(txt); return -1; }
            if (sb_put_ident(sql, keys[j].column) || sb_puts(sql, cmp) ||
                q_placeholder(q, sql) || q_push(q, txt)) { free(txt); return -1; }
        }
        if (sb_puts(sql, ")")) return -1;
    }
    return sb_puts(sql, ")");
}

static int build_keyset_order(sb_t *sql, const cel_sortkey_t *keys, int nk) {
    if (sb_puts(sql, " ORDER BY ")) return -1;
    for (int i = 0; i < nk; i++) {
        if (i && sb_puts(sql, ", ")) return -1;
        if (sb_put_ident(sql, keys[i].column)) return -1;
        if (sb_puts(sql, keys[i].desc ? " DESC" : " ASC")) return -1;
    }
    return 0;
}

static int build_limit_only(const cJSON *req, sb_t *sql, char *errbuf, size_t errlen) {
    long limit = CEL_LIST_DEFAULT_LIMIT;
    const cJSON *jl = req ? cJSON_GetObjectItemCaseSensitive(req, "limit") : NULL;
    if (jl) {
        if (!cJSON_IsNumber(jl)) FAIL("'limit' must be a number");
        limit = (long)jl->valuedouble;
        if (limit < 1) limit = 1;
        if (limit > CEL_LIST_MAX_LIMIT) limit = CEL_LIST_MAX_LIMIT;
    }
    char clause[40];
    snprintf(clause, sizeof clause, " LIMIT %ld", limit);
    return sb_puts(sql, clause);
}

/* ---- public builders ------------------------------------------------------- */

int cel_build_list(const cel_table_t *t, const cJSON *req, const cel_scope_t *scope,
                   const cJSON *cursor, cel_query_t *out, char *errbuf, size_t errlen) {
    sb_t sql;
    if (q_init(out) || sb_init(&sql)) { cel_query_free(out); FAIL("out of memory"); }

    int rc;
    if (cursor) {                                   /* keyset mode */
        cel_sortkey_t keys[CEL_MAX_SORTKEYS];
        int nk = cel_resolve_sortkeys(t, req, keys, CEL_MAX_SORTKEYS, errbuf, errlen);
        const cJSON *w = req ? cJSON_GetObjectItemCaseSensitive(req, "where") : NULL;
        bool emitted_where = (cJSON_IsObject(w) && cJSON_GetArraySize(w) > 0) || scope_count(scope) > 0;
        bool has_vals = cJSON_IsArray(cursor) && cJSON_GetArraySize(cursor) > 0;

        rc = nk < 0 ||
             sb_puts(&sql, "SELECT ") || build_select_list(t, req, &sql, errbuf, errlen) ||
             sb_puts(&sql, " FROM ") || sb_put_ident(&sql, t->name) ||
             build_where(t, req, scope, out, &sql, errbuf, errlen);
        if (!rc && has_vals)
            rc = sb_puts(&sql, emitted_where ? " AND " : " WHERE ") ||
                 build_keyset_predicate(out, &sql, keys, nk, cursor, errbuf, errlen);
        if (!rc)
            rc = build_keyset_order(&sql, keys, nk) || build_limit_only(req, &sql, errbuf, errlen);
    } else {                                        /* classic LIMIT/OFFSET */
        rc = sb_puts(&sql, "SELECT ") ||
             build_select_list(t, req, &sql, errbuf, errlen) ||
             sb_puts(&sql, " FROM ") || sb_put_ident(&sql, t->name) ||
             build_where(t, req, scope, out, &sql, errbuf, errlen) ||
             build_order(t, req, &sql, errbuf, errlen) ||
             build_limit_offset(req, &sql, errbuf, errlen);
    }
    if (rc) {
        free(sql.buf); cel_query_free(out);
        if (!errbuf[0]) snprintf(errbuf, errlen, "failed to build query");
        return -1;
    }
    out->sql = sql.buf;
    return 0;
}

/* Build a COUNT(*) over the same filters + scope as the list (no select/order/
 * limit). Reuses build_where verbatim, so the total is exactly the size of the
 * unpaginated result the caller is authorized to see. Returns one row, column
 * "count". */
int cel_build_count(const cel_table_t *t, const cJSON *req, const cel_scope_t *scope,
                    cel_query_t *out, char *errbuf, size_t errlen) {
    sb_t sql;
    if (q_init(out) || sb_init(&sql)) { cel_query_free(out); FAIL("out of memory"); }

    if (sb_puts(&sql, "SELECT count(*) AS count FROM ") || sb_put_ident(&sql, t->name) ||
        build_where(t, req, scope, out, &sql, errbuf, errlen)) {
        free(sql.buf); cel_query_free(out);
        if (!errbuf[0]) snprintf(errbuf, errlen, "failed to build count");
        return -1;
    }
    out->sql = sql.buf;
    return 0;
}

/* Whitelisted aggregate functions (anything else is rejected, never emitted). */
static const char *agg_func(const char *f) {
    if (!strcmp(f, "count")) return "count";
    if (!strcmp(f, "sum"))   return "sum";
    if (!strcmp(f, "avg"))   return "avg";
    if (!strcmp(f, "min"))   return "min";
    if (!strcmp(f, "max"))   return "max";
    return NULL;
}

/* Build the SELECT/group body of an aggregate into `sql`. Returns 0/-1 (errbuf). */
static int build_aggregate_body(const cel_table_t *t, const cJSON *req, const cel_scope_t *scope,
                                cel_query_t *q, sb_t *sql, char *errbuf, size_t errlen) {
    const cJSON *group = cJSON_GetObjectItemCaseSensitive(req, "group");
    const cJSON *aggs  = cJSON_GetObjectItemCaseSensitive(req, "aggregate");
    bool has_group = cJSON_IsArray(group) && cJSON_GetArraySize(group) > 0;
    bool has_agg   = cJSON_IsArray(aggs)  && cJSON_GetArraySize(aggs)  > 0;
    if (!has_group && !has_agg) FAIL("aggregate needs 'group' or 'aggregate'");

    if (sb_puts(sql, "SELECT ")) return -1;
    int n = 0;
    if (has_group) {
        const cJSON *g;
        cJSON_ArrayForEach(g, group) {
            if (!cJSON_IsString(g)) FAIL("'group' entries must be column names");
            if (!cel_table_column(t, g->valuestring)) FAIL("unknown column '%s' in group", g->valuestring);
            if (n++ && sb_puts(sql, ", ")) return -1;
            if (sb_put_ident(sql, g->valuestring)) return -1;
        }
    }
    if (has_agg) {
        const cJSON *a;
        cJSON_ArrayForEach(a, aggs) {
            if (!cJSON_IsString(a)) FAIL("'aggregate' entries must be strings");
            const char *spec = a->valuestring;
            const char *colon = strchr(spec, ':');
            char func[16];
            size_t flen = colon ? (size_t)(colon - spec) : strlen(spec);
            if (flen == 0 || flen >= sizeof func) FAIL("bad aggregate '%s'", spec);
            memcpy(func, spec, flen); func[flen] = '\0';
            const char *fn = agg_func(func);
            if (!fn) FAIL("unknown aggregate function '%s'", func);
            const char *col = colon ? colon + 1 : NULL;
            if (n++ && sb_puts(sql, ", ")) return -1;
            if (col && *col) {
                if (!cel_table_column(t, col)) FAIL("unknown column '%s' in aggregate", col);
                char alias[80];
                snprintf(alias, sizeof alias, "%s_%s", fn, col);
                if (sb_puts(sql, fn) || sb_puts(sql, "(") || sb_put_ident(sql, col) ||
                    sb_puts(sql, ") AS ") || sb_put_ident(sql, alias)) return -1;
            } else {
                if (strcmp(fn, "count")) FAIL("'%s' needs a column (e.g. %s:price)", fn, fn);
                if (sb_puts(sql, "count(*) AS ") || sb_put_ident(sql, "count")) return -1;
            }
        }
    }
    if (sb_puts(sql, " FROM ") || sb_put_ident(sql, t->name) ||
        build_where(t, req, scope, q, sql, errbuf, errlen)) return -1;

    if (has_group) {
        if (sb_puts(sql, " GROUP BY ")) return -1;
        int m = 0; const cJSON *g;
        cJSON_ArrayForEach(g, group) {
            if (m++ && sb_puts(sql, ", ")) return -1;
            if (sb_put_ident(sql, g->valuestring)) return -1;
        }
        if (sb_puts(sql, " ORDER BY ")) return -1;   /* deterministic ordering of groups */
        m = 0;
        cJSON_ArrayForEach(g, group) {
            if (m++ && sb_puts(sql, ", ")) return -1;
            if (sb_put_ident(sql, g->valuestring)) return -1;
        }
    }
    /* Bound the result like every other read path (H-2): a high-cardinality
     * group column would otherwise materialize a whole-table result in memory.
     * Honors an optional caller `limit`, clamped to CEL_LIST_MAX_LIMIT. */
    if (build_limit_offset(req, sql, errbuf, errlen)) return -1;
    return 0;
}

int cel_build_aggregate(const cel_table_t *t, const cJSON *req, const cel_scope_t *scope,
                        cel_query_t *out, char *errbuf, size_t errlen) {
    sb_t sql;
    if (q_init(out) || sb_init(&sql)) { cel_query_free(out); FAIL("out of memory"); }
    if (build_aggregate_body(t, req, scope, out, &sql, errbuf, errlen) != 0) {
        free(sql.buf); cel_query_free(out);
        if (!errbuf[0]) snprintf(errbuf, errlen, "failed to build aggregate");
        return -1;
    }
    out->sql = sql.buf;
    return 0;
}

int cel_build_get(const cel_table_t *t, const cJSON *req, const cel_scope_t *scope,
                  cel_query_t *out, char *errbuf, size_t errlen) {
    if (t->pk_index < 0) FAIL("table '%s' has no primary key", t->name);
    const cJSON *id = req ? cJSON_GetObjectItemCaseSensitive(req, "id") : NULL;
    if (!id || cJSON_IsNull(id)) FAIL("'id' is required");
    char *idtxt = json_scalar_text(id);
    if (!idtxt) FAIL("'id' must be a scalar value");

    sb_t sql;
    if (q_init(out) || sb_init(&sql)) { free(idtxt); cel_query_free(out); FAIL("out of memory"); }

    const char *pk = t->cols[t->pk_index].name;
    int rc = sb_puts(&sql, "SELECT ") ||
             build_select_list(t, req, &sql, errbuf, errlen) ||
             sb_puts(&sql, " FROM ") || sb_put_ident(&sql, t->name) ||
             sb_puts(&sql, " WHERE ") || sb_put_ident(&sql, pk) ||
             sb_puts(&sql, " = ") || q_placeholder(out, &sql) || q_push(out, idtxt);
    for (int i = 0; !rc && i < scope_count(scope); i++)
        rc = sb_puts(&sql, " AND ") || append_scope_predicate(t, out, &sql, &scope->rule[i]);
    if (rc) {
        free(sql.buf); free(idtxt); cel_query_free(out);
        if (!errbuf[0]) snprintf(errbuf, errlen, "failed to build query");
        return -1;
    }
    sb_puts(&sql, " LIMIT 1");
    out->sql = sql.buf;
    return 0;
}

/* ---- write builders -------------------------------------------------------- */

/* Append "RETURNING c1, c2, ..." for every column (so callers get the row back). */
static int append_returning_all(const cel_table_t *t, sb_t *sql) {
    if (sb_puts(sql, " RETURNING ")) return -1;
    for (int i = 0; i < t->ncols; i++) {
        if (i && sb_puts(sql, ", ")) return -1;
        if (sb_put_ident(sql, t->cols[i].name)) return -1;
    }
    return 0;
}

/* Emit a "?N" placeholder bound to a JSON value (null -> SQL NULL). */
static int push_value(cel_query_t *q, sb_t *sql, const cJSON *v,
                      const char *col, char *errbuf, size_t errlen) {
    if (q_placeholder(q, sql)) return -1;
    if (cJSON_IsNull(v)) return q_push(q, NULL);
    char *txt = json_scalar_text(v);
    if (!txt) FAIL("unsupported value for '%s'", col);
    return q_push(q, txt);
}

/* True if `col` is an EQ-forced scoped column (so a client-supplied value for it
 * is ignored on create / rejected on update). Only EQ rules force a column; OR
 * and VIA are read filters and never force/reject a column. */
static int is_scoped_col(const cel_scope_t *s, const char *col) {
    for (int i = 0; i < scope_count(s); i++)
        if (s->rule[i].kind == CEL_SCOPE_EQ && s->rule[i].column &&
            !strcmp(s->rule[i].column, col)) return 1;
    return 0;
}

int cel_build_create(const cel_table_t *t, const cJSON *req, const cel_scope_t *scope,
                     cel_query_t *out, char *errbuf, size_t errlen) {
    const cJSON *values = req ? cJSON_GetObjectItemCaseSensitive(req, "values") : NULL;
    if (!cJSON_IsObject(values) || cJSON_GetArraySize(values) == 0)
        FAIL("'values' object is required");

    /* validate every client column up front */
    const cJSON *m;
    cJSON_ArrayForEach(m, values)
        if (!cel_table_column(t, m->string)) FAIL("unknown column '%s'", m->string);

    int nscope = scope_count(scope);   /* scoped columns are forced, not client-supplied */

    /* SECURITY (H-9) defense-in-depth: only EQ scope is forceable on INSERT. OR/VIA
     * cannot be enforced at create time, and silently dropping them (as this builder
     * once did) emits an unscoped insert — letting the client spoof the owner column
     * or create an unowned row. make_scope already rejects an owner_any/owner_via
     * create policy; fail hard here too so no caller can drive a spoofable insert. */
    for (int i = 0; i < nscope; i++)
        if (scope->rule[i].kind != CEL_SCOPE_EQ)
            FAIL("create cannot enforce owner_any/owner_via scope on '%s' (use owner_column)", t->name);

    sb_t sql;
    if (q_init(out) || sb_init(&sql)) { cel_query_free(out); FAIL("out of memory"); }

    int rc = sb_puts(&sql, "INSERT INTO ") || sb_put_ident(&sql, t->name) || sb_puts(&sql, " (");
    int n = 0;
    cJSON_ArrayForEach(m, values) {
        if (is_scoped_col(scope, m->string)) continue;  /* ignore client-supplied scoped col */
        if (n++ && !rc) rc = sb_puts(&sql, ", ");
        if (!rc) rc = sb_put_ident(&sql, m->string);
    }
    for (int i = 0; !rc && i < nscope; i++) {
        if (scope->rule[i].kind != CEL_SCOPE_EQ) continue;  /* OR/VIA never forced on insert */
        if (n++) rc = sb_puts(&sql, ", ");
        if (!rc) rc = sb_put_ident(&sql, scope->rule[i].column);
    }
    if (!rc) rc = sb_puts(&sql, ") VALUES (");
    n = 0;
    cJSON_ArrayForEach(m, values) {
        if (is_scoped_col(scope, m->string)) continue;
        if (n++ && !rc) rc = sb_puts(&sql, ", ");
        if (!rc) rc = push_value(out, &sql, m, m->string, errbuf, errlen);
    }
    for (int i = 0; !rc && i < nscope; i++) {
        if (scope->rule[i].kind != CEL_SCOPE_EQ) continue;  /* OR/VIA never forced on insert */
        if (n++) rc = sb_puts(&sql, ", ");
        if (!rc) rc = append_value_param(out, &sql, scope->rule[i].value);
    }
    if (!rc) rc = sb_puts(&sql, ")") || append_returning_all(t, &sql);
    if (rc) { free(sql.buf); cel_query_free(out);
              if (!errbuf[0]) snprintf(errbuf, errlen, "failed to build query");
              return -1; }
    out->sql = sql.buf;
    return 0;
}

int cel_build_update(const cel_table_t *t, const cJSON *req, const cel_scope_t *scope,
                     cel_query_t *out, char *errbuf, size_t errlen) {
    if (t->pk_index < 0) FAIL("table '%s' has no primary key", t->name);
    const cJSON *values = req ? cJSON_GetObjectItemCaseSensitive(req, "values") : NULL;
    const cJSON *id     = req ? cJSON_GetObjectItemCaseSensitive(req, "id") : NULL;
    if (!cJSON_IsObject(values) || cJSON_GetArraySize(values) == 0)
        FAIL("'values' object is required");
    if (!id || cJSON_IsNull(id)) FAIL("'id' is required");

    const cJSON *m;
    cJSON_ArrayForEach(m, values) {
        if (!cel_table_column(t, m->string)) FAIL("unknown column '%s'", m->string);
        if (is_scoped_col(scope, m->string)) FAIL("cannot modify a scoped column");
    }

    sb_t sql;
    if (q_init(out) || sb_init(&sql)) { cel_query_free(out); FAIL("out of memory"); }

    int rc = sb_puts(&sql, "UPDATE ") || sb_put_ident(&sql, t->name) || sb_puts(&sql, " SET ");
    int n = 0;
    cJSON_ArrayForEach(m, values) {
        if (n++ && !rc) rc = sb_puts(&sql, ", ");
        if (!rc) rc = sb_put_ident(&sql, m->string) || sb_puts(&sql, " = ");
        if (!rc) rc = push_value(out, &sql, m, m->string, errbuf, errlen);
    }
    if (!rc) rc = sb_puts(&sql, " WHERE ") || sb_put_ident(&sql, t->cols[t->pk_index].name) ||
                  sb_puts(&sql, " = ");
    if (!rc) rc = push_value(out, &sql, id, "id", errbuf, errlen);
    for (int i = 0; !rc && i < scope_count(scope); i++)
        rc = sb_puts(&sql, " AND ") || append_scope_predicate(t, out, &sql, &scope->rule[i]);
    if (!rc) rc = append_returning_all(t, &sql);
    if (rc) { free(sql.buf); cel_query_free(out);
              if (!errbuf[0]) snprintf(errbuf, errlen, "failed to build query");
              return -1; }
    out->sql = sql.buf;
    return 0;
}

int cel_build_delete(const cel_table_t *t, const cJSON *req, const cel_scope_t *scope,
                     cel_query_t *out, char *errbuf, size_t errlen) {
    if (t->pk_index < 0) FAIL("table '%s' has no primary key", t->name);
    const cJSON *id = req ? cJSON_GetObjectItemCaseSensitive(req, "id") : NULL;
    if (!id || cJSON_IsNull(id)) FAIL("'id' is required");

    sb_t sql;
    if (q_init(out) || sb_init(&sql)) { cel_query_free(out); FAIL("out of memory"); }

    int rc = sb_puts(&sql, "DELETE FROM ") || sb_put_ident(&sql, t->name) ||
             sb_puts(&sql, " WHERE ") || sb_put_ident(&sql, t->cols[t->pk_index].name) ||
             sb_puts(&sql, " = ") || push_value(out, &sql, id, "id", errbuf, errlen);
    for (int i = 0; !rc && i < scope_count(scope); i++)
        rc = sb_puts(&sql, " AND ") || append_scope_predicate(t, out, &sql, &scope->rule[i]);
    if (!rc) rc = append_returning_all(t, &sql);
    if (rc) { free(sql.buf); cel_query_free(out);
              if (!errbuf[0]) snprintf(errbuf, errlen, "failed to build query");
              return -1; }
    out->sql = sql.buf;
    return 0;
}

int cel_build_soft_delete(const cel_table_t *t, const cJSON *req, const cel_scope_t *scope,
                          cel_query_t *out, char *errbuf, size_t errlen) {
    if (t->pk_index < 0) FAIL("table '%s' has no primary key", t->name);
    const cJSON *id   = req  ? cJSON_GetObjectItemCaseSensitive(req, "id") : NULL;
    const cJSON *vals = req  ? cJSON_GetObjectItemCaseSensitive(req, "values") : NULL;
    const cJSON *rev  = vals ? cJSON_GetObjectItemCaseSensitive(vals, "rev") : NULL;
    if (!id || cJSON_IsNull(id)) FAIL("'id' is required");
    if (!cJSON_IsNumber(rev))    FAIL("internal: soft-delete rev missing");

    sb_t sql;
    if (q_init(out) || sb_init(&sql)) { cel_query_free(out); FAIL("out of memory"); }

    /* engine-owned columns only (deleted literal, rev bound) → no scoped-column
     * guard; `AND deleted = 0` so a tombstone is never re-deleted (→ caller 404s). */
    int rc = sb_puts(&sql, "UPDATE ") || sb_put_ident(&sql, t->name) ||
             sb_puts(&sql, " SET deleted = 1, rev = ");
    if (!rc) rc = push_value(out, &sql, rev, "rev", errbuf, errlen);
    if (!rc) rc = sb_puts(&sql, " WHERE ") || sb_put_ident(&sql, t->cols[t->pk_index].name) ||
                  sb_puts(&sql, " = ");
    if (!rc) rc = push_value(out, &sql, id, "id", errbuf, errlen);
    if (!rc) rc = sb_puts(&sql, " AND deleted = 0");
    for (int i = 0; !rc && i < scope_count(scope); i++)
        rc = sb_puts(&sql, " AND ") || append_scope_predicate(t, out, &sql, &scope->rule[i]);
    if (!rc) rc = append_returning_all(t, &sql);
    if (rc) { free(sql.buf); cel_query_free(out);
              if (!errbuf[0]) snprintf(errbuf, errlen, "failed to build query");
              return -1; }
    out->sql = sql.buf;
    return 0;
}

int cel_build_pull(const cel_table_t *t, const cel_scope_t *scope, long long since,
                   long long limit, cel_query_t *out, char *errbuf, size_t errlen) {
    /* SELECT * FROM t WHERE rev > <since> [AND owner-scope] ORDER BY rev ASC LIMIT <limit>.
     * `since`/`limit` are engine-validated integers, inlined (%lld → injection-safe);
     * the scope passed in has had the tombstone rule stripped, so tombstones ARE
     * returned (a pull must propagate deletions). */
    sb_t sql;
    if (q_init(out) || sb_init(&sql)) { cel_query_free(out); FAIL("out of memory"); }
    char nbuf[32];
    int rc = sb_puts(&sql, "SELECT * FROM ") || sb_put_ident(&sql, t->name) ||
             sb_puts(&sql, " WHERE ") || sb_put_ident(&sql, "rev") || sb_puts(&sql, " > ");
    if (!rc) { snprintf(nbuf, sizeof nbuf, "%lld", since); rc = sb_puts(&sql, nbuf); }
    for (int i = 0; !rc && i < scope_count(scope); i++)
        rc = sb_puts(&sql, " AND ") || append_scope_predicate(t, out, &sql, &scope->rule[i]);
    if (!rc) rc = sb_puts(&sql, " ORDER BY ") || sb_put_ident(&sql, "rev") || sb_puts(&sql, " ASC LIMIT ");
    if (!rc) { snprintf(nbuf, sizeof nbuf, "%lld", limit); rc = sb_puts(&sql, nbuf); }
    if (rc) { free(sql.buf); cel_query_free(out);
              if (!errbuf[0]) snprintf(errbuf, errlen, "failed to build pull query");
              return -1; }
    out->sql = sql.buf;
    return 0;
}

/* NOTE (SQLite pivot): the emitted shape — SELECT * FROM fn(name := ?N) — is a
 * Postgres set-returning-function call with named args, which SQLite has no
 * equivalent for. RPC on SQLite is deferred to the hook layer (design §8:
 * rpc(name, args, who)) / Layer-1 SQL; this builder is kept for its identifier
 * validation and is not wired into the SQLite execution path. */
int cel_build_rpc(const char *fn, const cJSON *args,
                  cel_query_t *out, char *errbuf, size_t errlen) {
    if (!is_safe_ident(fn)) FAIL("invalid function name");

    sb_t sql;
    if (q_init(out) || sb_init(&sql)) { cel_query_free(out); FAIL("out of memory"); }

    int rc = sb_puts(&sql, "SELECT * FROM ") || sb_put_ident(&sql, fn) || sb_puts(&sql, "(");
    int n = 0;
    if (cJSON_IsObject(args)) {
        const cJSON *a;
        cJSON_ArrayForEach(a, args) {
            if (!is_safe_ident(a->string)) {
                free(sql.buf); cel_query_free(out);
                snprintf(errbuf, errlen, "invalid argument name '%s'", a->string ? a->string : "");
                return -1;
            }
            if (n++ && !rc) rc = sb_puts(&sql, ", ");
            /* "name := ?N" — name is charset-validated (emitted bare so it matches
             * the function's declared parameter); the value is always bound. */
            if (!rc) rc = sb_puts(&sql, a->string) || sb_puts(&sql, " := ");
            if (!rc) rc = push_value(out, &sql, a, a->string, errbuf, errlen);
        }
    }
    if (!rc) rc = sb_puts(&sql, ")");
    if (rc) { free(sql.buf); cel_query_free(out);
              if (!errbuf[0]) snprintf(errbuf, errlen, "failed to build query");
              return -1; }
    out->sql = sql.buf;
    return 0;
}
