/* cellar — hook value ABI test (Phase 2). Exercises the opaque-handle accessors
 * and mutators the Lua/FFI hook surface is built on: total reads (defaults on
 * absence/type-mismatch), object/array navigation, key iteration, and in-place
 * object mutation (before()'s `input` editing). cJSON builds the fixtures; the
 * code under test only ever sees cel_val_t*. */
#include "cel_val.h"
#include <cjson/cJSON.h>

#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
static void check(const char *name, int cond) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) fails++;
}

int main(void) {
    cJSON *root = cJSON_Parse(
        "{\"email\":\"a@b.c\",\"role\":\"admin\",\"age\":42,\"active\":true,"
        "\"nil\":null,\"tags\":[\"x\",\"y\",\"z\"],"
        "\"profile\":{\"city\":\"berlin\"}}");
    cel_val_t *v = (cel_val_t *)root;
    check("fixture parsed", v != NULL);
    if (!v) return 1;

    /* ---- type probe ---- */
    check("type(root) == OBJ",            cel_val_type(v) == CEL_V_OBJ);
    check("type(email) == STR",           cel_val_type(cel_val_get(v, "email")) == CEL_V_STR);
    check("type(age) == NUM",             cel_val_type(cel_val_get(v, "age")) == CEL_V_NUM);
    check("type(active) == BOOL",         cel_val_type(cel_val_get(v, "active")) == CEL_V_BOOL);
    check("type(nil) == NULL",            cel_val_type(cel_val_get(v, "nil")) == CEL_V_NULL);
    check("type(tags) == ARR",            cel_val_type(cel_val_get(v, "tags")) == CEL_V_ARR);
    check("type(NULL handle) == NULL",    cel_val_type(NULL) == CEL_V_NULL);

    /* ---- scalar reads ---- */
    check("str(email)",                   strcmp(cel_val_str(cel_val_get(v, "email")), "a@b.c") == 0);
    check("num(age) == 42",               fabs(cel_val_num(cel_val_get(v, "age")) - 42.0) < 1e-9);
    check("bool(active) == 1",            cel_val_bool(cel_val_get(v, "active")) == 1);

    /* ---- total reads: wrong type / absent → defaults, not crashes ---- */
    check("str(age) == NULL (not a str)", cel_val_str(cel_val_get(v, "age")) == NULL);
    check("num(email) == 0 (not a num)",  cel_val_num(cel_val_get(v, "email")) == 0.0);
    check("get(missing) == NULL",         cel_val_get(v, "nope") == NULL);
    check("str(NULL) == NULL",            cel_val_str(NULL) == NULL);

    /* ---- arrays ---- */
    const cel_val_t *tags = cel_val_get(v, "tags");
    check("len(tags) == 3",               cel_val_len(tags) == 3);
    check("at(tags,0) == 'x'",            strcmp(cel_val_str(cel_val_at(tags, 0)), "x") == 0);
    check("at(tags,2) == 'z'",            strcmp(cel_val_str(cel_val_at(tags, 2)), "z") == 0);
    check("at(tags,9) == NULL (oob)",     cel_val_at(tags, 9) == NULL);
    check("at(obj, i) == NULL",           cel_val_at(v, 0) == NULL);

    /* ---- nested object + key iteration ---- */
    check("get(profile.city)",            strcmp(cel_val_str(cel_val_get(cel_val_get(v, "profile"), "city")), "berlin") == 0);
    check("len(root) == 7 keys",          cel_val_len(v) == 7);
    check("key(root,0) == 'email'",       strcmp(cel_val_key(v, 0), "email") == 0);
    check("key(root,1) == 'role'",        strcmp(cel_val_key(v, 1), "role") == 0);
    check("key(root,99) == NULL (oob)",   cel_val_key(v, 99) == NULL);

    /* ---- mutation in place (the before() input-edit path) ---- */
    cel_val_set_str(v, "role", "viewer");           /* replace existing */
    check("set_str replaces",             strcmp(cel_val_str(cel_val_get(v, "role")), "viewer") == 0);
    cel_val_set_num(v, "score", 7);                 /* add new */
    check("set_num adds",                 cel_val_num(cel_val_get(v, "score")) == 7.0);
    cel_val_set_bool(v, "active", 0);
    check("set_bool replaces",            cel_val_bool(cel_val_get(v, "active")) == 0);
    cel_val_unset(v, "email");
    check("unset removes",                cel_val_get(v, "email") == NULL);
    cel_val_set_null(v, "score");
    check("set_null sets null",           cel_val_type(cel_val_get(v, "score")) == CEL_V_NULL);

    /* ---- mutators are no-ops on non-objects / NULL (no crash) ---- */
    cel_val_set_str((cel_val_t *)tags, "k", "v");   /* tags is an array */
    check("set on array is a no-op",      cel_val_get((cel_val_t *)tags, "k") == NULL);
    cel_val_set_str(NULL, "k", "v");                /* must not crash */
    cel_val_unset(NULL, "k");
    check("mutate NULL handle survives",  1);

    cJSON_Delete(root);
    printf("\n%s  (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
