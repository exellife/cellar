/* cellar — query-builder regression test (no DB).
 *
 * Pins the EXACT SQL the builder emits for the single-tenant (Model A) cases —
 * unscoped, and owner-scoped (one scope rule). This is the safety net for the
 * Phase 7 scope-rule generalization (#42/#44): if turning on tenant scoping (or
 * any future change) ever alters the SQL a single-tenant deployment produces,
 * this test fails loudly. Builds against query_builder.c alone (cel_table_column
 * is stubbed below), so it needs neither Postgres nor the rest of the engine.
 */
#include "query_builder.h"
#include "schema_catalog.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>

/* Stub the one external symbol query_builder.c needs — identical semantics to
 * the real catalog lookup, but over a hand-built table. */
const cel_column_t *cel_table_column(const cel_table_t *t, const char *column) {
    for (int i = 0; i < t->ncols; i++)
        if (!strcmp(t->cols[i].name, column)) return &t->cols[i];
    return NULL;
}

static int failures = 0;

/* {op: val} */
static cJSON *opobj(const char *op, const char *val) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, op, val);
    return o;
}
/* {col: {op: val}} — a single-column condition node */
static cJSON *cond(const char *col, const char *op, const char *val) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, col, opobj(op, val));
    return o;
}

static void expect(const char *label, int rc, const cel_query_t *q,
                   const char *want_sql, int want_nparams) {
    if (rc != 0) { printf("  FAIL %-22s builder returned error\n", label); failures++; return; }
    int ok = !strcmp(q->sql, want_sql) && q->nparams == want_nparams;
    if (ok) { printf("  ok   %-22s\n", label); return; }
    printf("  FAIL %-22s\n      got : %s  [nparams=%d]\n      want: %s  [nparams=%d]\n",
           label, q->sql, q->nparams, want_sql, want_nparams);
    failures++;
}

/* Expect the builder to REJECT (nonzero rc): a misconfiguration that must fail
 * closed instead of silently emitting unsafe SQL. */
static void expect_reject(const char *label, int rc) {
    if (rc != 0) { printf("  ok   %-22s (rejected)\n", label); return; }
    printf("  FAIL %-22s expected rejection, built SQL\n", label); failures++;
}

int main(void) {
    /* in-memory table: notes(id uuid PK, owner_id uuid, title text) */
    cel_column_t cols[3];
    memset(cols, 0, sizeof cols);
    snprintf(cols[0].name, sizeof cols[0].name, "id");       cols[0].type = CEL_T_UUID; cols[0].is_pk = true;
    snprintf(cols[1].name, sizeof cols[1].name, "owner_id"); cols[1].type = CEL_T_UUID;
    snprintf(cols[2].name, sizeof cols[2].name, "title");    cols[2].type = CEL_T_TEXT;
    cel_table_t t;
    memset(&t, 0, sizeof t);
    snprintf(t.name, sizeof t.name, "notes");
    t.cols = cols; t.ncols = 3; t.pk_index = 0;

    /* Model A, owner-scoped: a single rule (owner_id = the caller's user id). */
    cel_scope_t owner = { .count = 1 };
    owner.rule[0].column = "owner_id";
    owner.rule[0].value  = "u1";

    /* Model A, no scoping (no ownership policy on the table). */
    cel_scope_t none = { .count = 0 };

    char err[256];
    cel_query_t q;
    cJSON *req;

    printf("query-builder SQL regression (single-tenant / Model A)\n");

    /* ---- unscoped ---- */
    req = cJSON_CreateObject();
    expect("list unscoped", cel_build_list(&t, req, &none, NULL, &q, err, sizeof err), &q,
           "SELECT \"id\", \"owner_id\", \"title\" FROM \"notes\" LIMIT 100 OFFSET 0", 0);
    cel_query_free(&q); cJSON_Delete(req);

    /* ---- owner-scoped (the case the #42 refactor must not change) ---- */
    req = cJSON_CreateObject();
    expect("list owner", cel_build_list(&t, req, &owner, NULL, &q, err, sizeof err), &q,
           "SELECT \"id\", \"owner_id\", \"title\" FROM \"notes\" WHERE \"owner_id\" = ?1 LIMIT 100 OFFSET 0", 1);
    cel_query_free(&q); cJSON_Delete(req);

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "id", "x1");
    expect("get owner", cel_build_get(&t, req, &owner, &q, err, sizeof err), &q,
           "SELECT \"id\", \"owner_id\", \"title\" FROM \"notes\" WHERE \"id\" = ?1 AND \"owner_id\" = ?2 LIMIT 1", 2);
    cel_query_free(&q); cJSON_Delete(req);

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(cJSON_AddObjectToObject(req, "values"), "title", "hi");
    expect("create owner", cel_build_create(&t, req, &owner, &q, err, sizeof err), &q,
           "INSERT INTO \"notes\" (\"title\", \"owner_id\") VALUES (?1, ?2) RETURNING \"id\", \"owner_id\", \"title\"", 2);
    cel_query_free(&q); cJSON_Delete(req);

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "id", "x1");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(req, "values"), "title", "hi");
    expect("update owner", cel_build_update(&t, req, &owner, &q, err, sizeof err), &q,
           "UPDATE \"notes\" SET \"title\" = ?1 WHERE \"id\" = ?2 AND \"owner_id\" = ?3 RETURNING \"id\", \"owner_id\", \"title\"", 3);
    cel_query_free(&q); cJSON_Delete(req);

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "id", "x1");
    expect("delete owner", cel_build_delete(&t, req, &owner, &q, err, sizeof err), &q,
           "DELETE FROM \"notes\" WHERE \"id\" = ?1 AND \"owner_id\" = ?2 RETURNING \"id\", \"owner_id\", \"title\"", 2);
    cel_query_free(&q); cJSON_Delete(req);

    /* A client-supplied value for the scoped column is ignored (forced), not trusted. */
    req = cJSON_CreateObject();
    cJSON *vals = cJSON_AddObjectToObject(req, "values");
    cJSON_AddStringToObject(vals, "title", "hi");
    cJSON_AddStringToObject(vals, "owner_id", "ATTACKER");   /* must be dropped */
    expect("create ignores client owner", cel_build_create(&t, req, &owner, &q, err, sizeof err), &q,
           "INSERT INTO \"notes\" (\"title\", \"owner_id\") VALUES (?1, ?2) RETURNING \"id\", \"owner_id\", \"title\"", 2);
    /* and the forced value is the caller's, not the attacker's */
    if (q.nparams == 2 && strcmp(q.params[1], "u1") != 0) {
        printf("  FAIL create forces caller owner   got owner param=%s want u1\n", q.params[1]);
        failures++;
    } else if (q.nparams == 2) {
        printf("  ok   create forces caller owner\n");
    }
    cel_query_free(&q); cJSON_Delete(req);

    /* ---- pooled mode (Model B): tenant + owner scope compose as two AND-ed
     * rules, and CREATE forces both columns. Table carries a tenant_id column. */
    cel_column_t pcols[4];
    memset(pcols, 0, sizeof pcols);
    snprintf(pcols[0].name, sizeof pcols[0].name, "id");        pcols[0].type = CEL_T_UUID; pcols[0].is_pk = true;
    snprintf(pcols[1].name, sizeof pcols[1].name, "tenant_id"); pcols[1].type = CEL_T_UUID;
    snprintf(pcols[2].name, sizeof pcols[2].name, "owner_id");  pcols[2].type = CEL_T_UUID;
    snprintf(pcols[3].name, sizeof pcols[3].name, "title");     pcols[3].type = CEL_T_TEXT;
    cel_table_t pt;
    memset(&pt, 0, sizeof pt);
    snprintf(pt.name, sizeof pt.name, "notes");
    pt.cols = pcols; pt.ncols = 4; pt.pk_index = 0;

    cel_scope_t pooled = { .count = 2 };          /* tenant first, then owner */
    pooled.rule[0].column = "tenant_id"; pooled.rule[0].value = "t1";
    pooled.rule[1].column = "owner_id";  pooled.rule[1].value = "u1";

    req = cJSON_CreateObject();
    expect("list tenant+owner", cel_build_list(&pt, req, &pooled, NULL, &q, err, sizeof err), &q,
           "SELECT \"id\", \"tenant_id\", \"owner_id\", \"title\" FROM \"notes\" "
           "WHERE \"tenant_id\" = ?1 AND \"owner_id\" = ?2 LIMIT 100 OFFSET 0", 2);
    cel_query_free(&q); cJSON_Delete(req);

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(cJSON_AddObjectToObject(req, "values"), "title", "hi");
    expect("create forces tenant+owner", cel_build_create(&pt, req, &pooled, &q, err, sizeof err), &q,
           "INSERT INTO \"notes\" (\"title\", \"tenant_id\", \"owner_id\") VALUES (?1, ?2, ?3) "
           "RETURNING \"id\", \"tenant_id\", \"owner_id\", \"title\"", 3);
    /* the forced values are the caller's tenant + user, in order */
    if (q.nparams == 3 && (strcmp(q.params[1], "t1") || strcmp(q.params[2], "u1"))) {
        printf("  FAIL create forces caller t/u    got [%s,%s] want [t1,u1]\n", q.params[1], q.params[2]);
        failures++;
    } else if (q.nparams == 3) {
        printf("  ok   create forces caller t/u\n");
    }
    cel_query_free(&q); cJSON_Delete(req);

    /* ---- #52 generalized access rules: OR + VIA (relationship) scoping ---- */

    /* trips(id, tenant_id, rider_id, driver_id, title) */
    cel_column_t tcols[5];
    memset(tcols, 0, sizeof tcols);
    snprintf(tcols[0].name, sizeof tcols[0].name, "id");        tcols[0].type = CEL_T_UUID; tcols[0].is_pk = true;
    snprintf(tcols[1].name, sizeof tcols[1].name, "tenant_id"); tcols[1].type = CEL_T_UUID;
    snprintf(tcols[2].name, sizeof tcols[2].name, "rider_id");  tcols[2].type = CEL_T_UUID;
    snprintf(tcols[3].name, sizeof tcols[3].name, "driver_id"); tcols[3].type = CEL_T_UUID;
    snprintf(tcols[4].name, sizeof tcols[4].name, "title");     tcols[4].type = CEL_T_TEXT;
    cel_table_t tt; memset(&tt, 0, sizeof tt);
    snprintf(tt.name, sizeof tt.name, "trips");
    tt.cols = tcols; tt.ncols = 5; tt.pk_index = 0;

    /* OR: a single caller matches via rider_id OR driver_id */
    cel_scope_t or_s = { .count = 1 };
    or_s.rule[0].kind = CEL_SCOPE_OR; or_s.rule[0].value = "u1";
    or_s.rule[0].cols[0] = "rider_id"; or_s.rule[0].cols[1] = "driver_id"; or_s.rule[0].ncols = 2;

    req = cJSON_CreateObject();
    expect("list OR", cel_build_list(&tt, req, &or_s, NULL, &q, err, sizeof err), &q,
           "SELECT \"id\", \"tenant_id\", \"rider_id\", \"driver_id\", \"title\" FROM \"trips\" "
           "WHERE (\"rider_id\" = ?1 OR \"driver_id\" = ?2) LIMIT 100 OFFSET 0", 2);
    cel_query_free(&q); cJSON_Delete(req);

    /* OR cannot be enforced on CREATE — a create policy using owner_any is a
     * misconfiguration; the builder must REJECT it, not silently emit an unscoped
     * insert that lets the client spoof the owner column (H-9). */
    req = cJSON_CreateObject();
    cJSON_AddStringToObject(cJSON_AddObjectToObject(req, "values"), "title", "hi");
    expect_reject("create rejects OR", cel_build_create(&tt, req, &or_s, &q, err, sizeof err));
    cel_query_free(&q); cJSON_Delete(req);

    /* tenant EQ + OR compose (AND), with correct $N numbering across both */
    cel_scope_t tor = { .count = 2 };
    tor.rule[0].kind = CEL_SCOPE_EQ; tor.rule[0].column = "tenant_id"; tor.rule[0].value = "t1";
    tor.rule[1].kind = CEL_SCOPE_OR; tor.rule[1].value = "u1";
    tor.rule[1].cols[0] = "rider_id"; tor.rule[1].cols[1] = "driver_id"; tor.rule[1].ncols = 2;

    req = cJSON_CreateObject();
    expect("list tenant+OR", cel_build_list(&tt, req, &tor, NULL, &q, err, sizeof err), &q,
           "SELECT \"id\", \"tenant_id\", \"rider_id\", \"driver_id\", \"title\" FROM \"trips\" "
           "WHERE \"tenant_id\" = ?1 AND (\"rider_id\" = ?2 OR \"driver_id\" = ?3) LIMIT 100 OFFSET 0", 3);
    cel_query_free(&q); cJSON_Delete(req);

    /* VIA: messages(id, trip_id, body) scoped by membership in trip_parts */
    cel_column_t mcols[3];
    memset(mcols, 0, sizeof mcols);
    snprintf(mcols[0].name, sizeof mcols[0].name, "id");      mcols[0].type = CEL_T_UUID; mcols[0].is_pk = true;
    snprintf(mcols[1].name, sizeof mcols[1].name, "trip_id"); mcols[1].type = CEL_T_UUID;
    snprintf(mcols[2].name, sizeof mcols[2].name, "body");    mcols[2].type = CEL_T_TEXT;
    cel_table_t mt; memset(&mt, 0, sizeof mt);
    snprintf(mt.name, sizeof mt.name, "messages");
    mt.cols = mcols; mt.ncols = 3; mt.pk_index = 0;

    cel_scope_t via = { .count = 1 };
    via.rule[0].kind = CEL_SCOPE_VIA; via.rule[0].value = "u1";
    via.rule[0].via_table = "trip_parts"; via.rule[0].via_ref = "trip_id";
    via.rule[0].via_local = "trip_id";    via.rule[0].via_user = "user_id";

    req = cJSON_CreateObject();
    expect("list VIA", cel_build_list(&mt, req, &via, NULL, &q, err, sizeof err), &q,
           "SELECT \"id\", \"trip_id\", \"body\" FROM \"messages\" "
           "WHERE EXISTS (SELECT 1 FROM \"trip_parts\" WHERE \"trip_parts\".\"trip_id\" = \"messages\".\"trip_id\" "
           "AND \"trip_parts\".\"user_id\" = ?1) LIMIT 100 OFFSET 0", 1);
    cel_query_free(&q); cJSON_Delete(req);

    /* VIA also confines DELETE (you can only delete a row you can see) */
    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "id", "x1");
    expect("delete VIA", cel_build_delete(&mt, req, &via, &q, err, sizeof err), &q,
           "DELETE FROM \"messages\" WHERE \"id\" = ?1 AND EXISTS (SELECT 1 FROM \"trip_parts\" "
           "WHERE \"trip_parts\".\"trip_id\" = \"messages\".\"trip_id\" AND \"trip_parts\".\"user_id\" = ?2) "
           "RETURNING \"id\", \"trip_id\", \"body\"", 2);
    cel_query_free(&q); cJSON_Delete(req);

    /* VIA membership cannot be established at insert time either — reject on CREATE
     * rather than create a row with no ownership linkage (H-9). */
    req = cJSON_CreateObject();
    cJSON_AddStringToObject(cJSON_AddObjectToObject(req, "values"), "body", "hi");
    expect_reject("create rejects VIA", cel_build_create(&mt, req, &via, &q, err, sizeof err));
    cel_query_free(&q); cJSON_Delete(req);

    /* ---- where-tree edge cases: a node that emits no condition must be REJECTED
     * (an empty object -> `()` / `NOT ()` is malformed SQL -> a 500), and the tree
     * depth is capped (a pathological nest is a clean 400, not a stack overflow). ---- */

    /* positive control: a valid nested where builds correctly */
    req = cJSON_CreateObject();
    cJSON *wtree = cJSON_AddObjectToObject(req, "where");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(cJSON_AddObjectToObject(wtree, "not"), "title"), "eq", "x");
    expect("where not(eq)", cel_build_list(&t, req, &none, NULL, &q, err, sizeof err), &q,
           "SELECT \"id\", \"owner_id\", \"title\" FROM \"notes\" WHERE NOT (\"title\" = ?1) LIMIT 100 OFFSET 0", 1);
    cel_query_free(&q); cJSON_Delete(req);

    /* empty `not` object -> reject (was: NOT () -> SQLite syntax error -> 500) */
    req = cJSON_CreateObject();
    cJSON_AddObjectToObject(cJSON_AddObjectToObject(req, "where"), "not");
    expect_reject("where not{} rejected", cel_build_list(&t, req, &none, NULL, &q, err, sizeof err));
    cel_query_free(&q); cJSON_Delete(req);

    /* `and` with an empty-object entry -> reject (was: () -> 500) */
    req = cJSON_CreateObject();
    cJSON_AddItemToArray(cJSON_AddArrayToObject(cJSON_AddObjectToObject(req, "where"), "and"), cJSON_CreateObject());
    expect_reject("where and[{}] rejected", cel_build_list(&t, req, &none, NULL, &q, err, sizeof err));
    cel_query_free(&q); cJSON_Delete(req);

    /* over-deep nesting (>32) -> reject, not a stack overflow */
    req = cJSON_CreateObject();
    cJSON *wnode = cJSON_AddObjectToObject(req, "where");
    for (int i = 0; i < 40; i++) wnode = cJSON_AddObjectToObject(wnode, "not");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(wnode, "title"), "eq", "x");
    expect_reject("where over-deep rejected", cel_build_list(&t, req, &none, NULL, &q, err, sizeof err));
    cel_query_free(&q); cJSON_Delete(req);

    /* ---- #54 RPC builder ---- */
    {
        cel_query_t qq; char e[256];
        memset(&qq, 0, sizeof qq);
        expect("rpc no args", cel_build_rpc("do_thing", NULL, &qq, e, sizeof e), &qq,
               "SELECT * FROM \"do_thing\"()", 0);
        cel_query_free(&qq);
    }
    {
        cJSON *args = cJSON_CreateObject();
        cJSON_AddNumberToObject(args, "a", 2);
        cJSON_AddNumberToObject(args, "b", 40);
        cel_query_t qq; char e[256];
        memset(&qq, 0, sizeof qq);
        expect("rpc named args", cel_build_rpc("add_two", args, &qq, e, sizeof e), &qq,
               "SELECT * FROM \"add_two\"(a := ?1, b := ?2)", 2);
        cel_query_free(&qq);
        cJSON_Delete(args);
    }
    {   /* an unsafe function name is rejected, never emitted */
        cel_query_t qq; char e[256]; memset(&qq, 0, sizeof qq);
        int rc = cel_build_rpc("bad-name; DROP", NULL, &qq, e, sizeof e);
        if (rc != 0) printf("  ok   rpc rejects bad fn\n");
        else { printf("  FAIL rpc rejects bad fn\n"); failures++; }
    }
    {   /* an unsafe argument name is rejected */
        cJSON *args = cJSON_CreateObject();
        cJSON_AddNumberToObject(args, "a) ; DROP", 1);
        cel_query_t qq; char e[256]; memset(&qq, 0, sizeof qq);
        int rc = cel_build_rpc("ok_fn", args, &qq, e, sizeof e);
        if (rc != 0) printf("  ok   rpc rejects bad arg\n");
        else { printf("  FAIL rpc rejects bad arg\n"); failures++; }
        cJSON_Delete(args);
    }

    /* ---- richer read: COUNT(*) shares the list's WHERE + scope, no order/limit ---- */
    req = cJSON_CreateObject();
    expect("count unscoped", cel_build_count(&t, req, &none, &q, err, sizeof err), &q,
           "SELECT count(*) AS count FROM \"notes\"", 0);
    cel_query_free(&q); cJSON_Delete(req);

    req = cJSON_CreateObject();
    expect("count owner-scoped", cel_build_count(&t, req, &owner, &q, err, sizeof err), &q,
           "SELECT count(*) AS count FROM \"notes\" WHERE \"owner_id\" = ?1", 1);
    cel_query_free(&q); cJSON_Delete(req);

    /* a filter on the list is reflected in the count (so total matches the result set) */
    req = cJSON_CreateObject();
    cJSON_AddStringToObject(cJSON_AddObjectToObject(cJSON_AddObjectToObject(req, "where"), "title"), "eq", "hi");
    expect("count with filter+scope", cel_build_count(&t, req, &owner, &q, err, sizeof err), &q,
           "SELECT count(*) AS count FROM \"notes\" WHERE \"title\" = ?1 AND \"owner_id\" = ?2", 2);
    cel_query_free(&q); cJSON_Delete(req);

    /* ---- keyset pagination: ORDER BY + PK tiebreaker, no OFFSET ---- */
    cJSON *empty = cJSON_CreateArray();

    /* first page, no order -> keyset on the PK alone */
    req = cJSON_CreateObject();
    expect("keyset first (pk)", cel_build_list(&t, req, &none, empty, &q, err, sizeof err), &q,
           "SELECT \"id\", \"owner_id\", \"title\" FROM \"notes\" ORDER BY \"id\" ASC LIMIT 100", 0);
    cel_query_free(&q); cJSON_Delete(req);

    /* first page, order=-title -> (title DESC, id DESC); PK appended same direction */
    req = cJSON_CreateObject();
    cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "order"), cJSON_CreateString("-title"));
    expect("keyset first (order)", cel_build_list(&t, req, &none, empty, &q, err, sizeof err), &q,
           "SELECT \"id\", \"owner_id\", \"title\" FROM \"notes\" ORDER BY \"title\" DESC, \"id\" DESC LIMIT 100", 0);
    cel_query_free(&q); cJSON_Delete(req);
    cJSON_Delete(empty);

    /* next page: cursor predicate (lexicographic, expanded) */
    req = cJSON_CreateObject();
    cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "order"), cJSON_CreateString("-title"));
    cJSON *cur = cJSON_CreateArray();
    cJSON_AddItemToArray(cur, cJSON_CreateString("hello"));
    cJSON_AddItemToArray(cur, cJSON_CreateString("u9"));
    expect("keyset next page", cel_build_list(&t, req, &none, cur, &q, err, sizeof err), &q,
           "SELECT \"id\", \"owner_id\", \"title\" FROM \"notes\" "
           "WHERE ((\"title\" < ?1) OR (\"title\" = ?2 AND \"id\" < ?3)) "
           "ORDER BY \"title\" DESC, \"id\" DESC LIMIT 100", 3);
    cel_query_free(&q); cJSON_Delete(req); cJSON_Delete(cur);

    /* next page + owner scope: scope AND keyset predicate */
    req = cJSON_CreateObject();
    cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "order"), cJSON_CreateString("-title"));
    cJSON *cur2 = cJSON_CreateArray();
    cJSON_AddItemToArray(cur2, cJSON_CreateString("hello"));
    cJSON_AddItemToArray(cur2, cJSON_CreateString("u9"));
    expect("keyset + owner scope", cel_build_list(&t, req, &owner, cur2, &q, err, sizeof err), &q,
           "SELECT \"id\", \"owner_id\", \"title\" FROM \"notes\" "
           "WHERE \"owner_id\" = ?1 AND ((\"title\" < ?2) OR (\"title\" = ?3 AND \"id\" < ?4)) "
           "ORDER BY \"title\" DESC, \"id\" DESC LIMIT 100", 4);
    cel_query_free(&q); cJSON_Delete(req); cJSON_Delete(cur2);

    /* ---- boolean filter tree: and / or / not / between ---- */
    cJSON *where;

    /* between */
    req = cJSON_CreateObject();
    where = cJSON_AddObjectToObject(req, "where");
    cJSON *bt = cJSON_AddArrayToObject(cJSON_AddObjectToObject(where, "title"), "between");
    cJSON_AddItemToArray(bt, cJSON_CreateString("a"));
    cJSON_AddItemToArray(bt, cJSON_CreateString("z"));
    expect("where between", cel_build_list(&t, req, &none, NULL, &q, err, sizeof err), &q,
           "SELECT \"id\", \"owner_id\", \"title\" FROM \"notes\" WHERE \"title\" BETWEEN ?1 AND ?2 LIMIT 100 OFFSET 0", 2);
    cel_query_free(&q); cJSON_Delete(req);

    /* or */
    req = cJSON_CreateObject();
    where = cJSON_AddObjectToObject(req, "where");
    cJSON *orarr = cJSON_AddArrayToObject(where, "or");
    cJSON_AddItemToArray(orarr, cond("title", "eq", "a"));
    cJSON_AddItemToArray(orarr, cond("owner_id", "eq", "b"));
    expect("where OR", cel_build_list(&t, req, &none, NULL, &q, err, sizeof err), &q,
           "SELECT \"id\", \"owner_id\", \"title\" FROM \"notes\" WHERE (\"title\" = ?1 OR \"owner_id\" = ?2) LIMIT 100 OFFSET 0", 2);
    cel_query_free(&q); cJSON_Delete(req);

    /* not */
    req = cJSON_CreateObject();
    where = cJSON_AddObjectToObject(req, "where");
    cJSON_AddItemToObject(where, "not", cond("title", "eq", "x"));   /* NOT ({title:{eq}}) */
    expect("where NOT", cel_build_list(&t, req, &none, NULL, &q, err, sizeof err), &q,
           "SELECT \"id\", \"owner_id\", \"title\" FROM \"notes\" WHERE NOT (\"title\" = ?1) LIMIT 100 OFFSET 0", 1);
    cel_query_free(&q); cJSON_Delete(req);

    /* mixed: a column AND an or-group (column key emitted first, then the group) */
    req = cJSON_CreateObject();
    where = cJSON_AddObjectToObject(req, "where");
    cJSON_AddItemToObject(where, "title", opobj("eq", "t"));
    cJSON *orarr2 = cJSON_AddArrayToObject(where, "or");
    cJSON_AddItemToArray(orarr2, cond("owner_id", "eq", "a"));
    cJSON_AddItemToArray(orarr2, cond("owner_id", "eq", "b"));
    expect("where mixed col+OR", cel_build_list(&t, req, &none, NULL, &q, err, sizeof err), &q,
           "SELECT \"id\", \"owner_id\", \"title\" FROM \"notes\" "
           "WHERE \"title\" = ?1 AND (\"owner_id\" = ?2 OR \"owner_id\" = ?3) LIMIT 100 OFFSET 0", 3);
    cel_query_free(&q); cJSON_Delete(req);

    /* nested AND inside OR */
    req = cJSON_CreateObject();
    where = cJSON_AddObjectToObject(req, "where");
    cJSON *oarr = cJSON_AddArrayToObject(where, "or");
    cJSON *andnode = cJSON_CreateObject();
    cJSON *aarr = cJSON_AddArrayToObject(andnode, "and");
    cJSON_AddItemToArray(aarr, cond("title", "eq", "a"));
    cJSON_AddItemToArray(aarr, cond("owner_id", "eq", "b"));
    cJSON_AddItemToArray(oarr, andnode);
    cJSON_AddItemToArray(oarr, cond("title", "eq", "c"));
    expect("where nested and/or", cel_build_list(&t, req, &none, NULL, &q, err, sizeof err), &q,
           "SELECT \"id\", \"owner_id\", \"title\" FROM \"notes\" "
           "WHERE ((\"title\" = ?1 AND \"owner_id\" = ?2) OR \"title\" = ?3) LIMIT 100 OFFSET 0", 3);
    cel_query_free(&q); cJSON_Delete(req);

    /* ---- group-by aggregates ---- */

    /* group + count */
    req = cJSON_CreateObject();
    cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "group"), cJSON_CreateString("owner_id"));
    cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "aggregate"), cJSON_CreateString("count"));
    expect("agg group+count", cel_build_aggregate(&t, req, &none, &q, err, sizeof err), &q,
           "SELECT \"owner_id\", count(*) AS \"count\" FROM \"notes\" GROUP BY \"owner_id\" ORDER BY \"owner_id\" LIMIT 100 OFFSET 0", 0);
    cel_query_free(&q); cJSON_Delete(req);

    /* group + count + max(col) */
    req = cJSON_CreateObject();
    cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "group"), cJSON_CreateString("owner_id"));
    cJSON *aggl = cJSON_AddArrayToObject(req, "aggregate");
    cJSON_AddItemToArray(aggl, cJSON_CreateString("count"));
    cJSON_AddItemToArray(aggl, cJSON_CreateString("max:title"));
    expect("agg group+count+max", cel_build_aggregate(&t, req, &none, &q, err, sizeof err), &q,
           "SELECT \"owner_id\", count(*) AS \"count\", max(\"title\") AS \"max_title\" "
           "FROM \"notes\" GROUP BY \"owner_id\" ORDER BY \"owner_id\" LIMIT 100 OFFSET 0", 0);
    cel_query_free(&q); cJSON_Delete(req);

    /* aggregate only -> a single totals row, no GROUP BY */
    req = cJSON_CreateObject();
    cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "aggregate"), cJSON_CreateString("count"));
    expect("agg totals (no group)", cel_build_aggregate(&t, req, &none, &q, err, sizeof err), &q,
           "SELECT count(*) AS \"count\" FROM \"notes\" LIMIT 100 OFFSET 0", 0);
    cel_query_free(&q); cJSON_Delete(req);

    /* group + count under owner scope (totals are confined to the caller's rows) */
    req = cJSON_CreateObject();
    cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "group"), cJSON_CreateString("owner_id"));
    cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "aggregate"), cJSON_CreateString("count"));
    expect("agg group+count+scope", cel_build_aggregate(&t, req, &owner, &q, err, sizeof err), &q,
           "SELECT \"owner_id\", count(*) AS \"count\" FROM \"notes\" WHERE \"owner_id\" = ?1 "
           "GROUP BY \"owner_id\" ORDER BY \"owner_id\" LIMIT 100 OFFSET 0", 1);
    cel_query_free(&q); cJSON_Delete(req);

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
