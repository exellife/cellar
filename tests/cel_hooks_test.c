/* cellar — Lua hook dispatcher test (Phase 2). Proves option A end-to-end inside
 * the engine: payloads cross as opaque cel_val_t* handles, the `cellar` prelude
 * boxes them into table-like proxies (who.role, input.title), before() mutates
 * its input in place, authorize() allows/denies, absent hooks are no-ops, and a
 * faulting hook fails closed — all reached via ffi.C into cellar's own symbols
 * (so this binary is linked -rdynamic). cJSON builds fixtures; the engine sees
 * only cel_val_t*. */
#include "cel_hooks.h"
#include "cel_lua.h"
#include "cel_val.h"
#include <cjson/cJSON.h>

#include <stdio.h>
#include <string.h>

static int fails = 0;
static void check(const char *name, int cond, const char *detail) {
    printf("  %-4s %-40s %s\n", cond ? "ok" : "FAIL", name, detail ? detail : "");
    if (!cond) fails++;
}

/* open a state, install the prelude, load an optional hooks.lua */
static cel_lua_t *make(const char *hooks) {
    char err[256];
    cel_lua_t *L = cel_lua_open();
    if (!L) return NULL;
    if (cel_hooks_install(L, err, sizeof err)) { fprintf(stderr, "install: %s\n", err); cel_lua_close(L); return NULL; }
    if (hooks && cel_lua_dostring(L, hooks, err, sizeof err)) { fprintf(stderr, "hooks: %s\n", err); cel_lua_close(L); return NULL; }
    return L;
}

static const char *HOOKS =
    "function authorize(op, tbl, row, who)\n"
    "  if tbl == 'secrets' and who.role ~= 'admin' then return false end\n"
    "  return true\n"
    "end\n"
    "function before(op, tbl, input, who)\n"
    "  cellar.log.info('before ' .. tbl .. ' by ' .. tostring(who.role))\n"
    "  if tbl == 'notes' and op == 'create' then\n"
    "    if not input.title or #input.title == 0 then return false, 'title is required' end\n"
    "    input.title = (input.title:gsub('^%s+', ''))   -- trim leading space, in place\n"
    "    input.normalized = true\n"
    "    input.scratch = nil                            -- nil removes the key\n"
    "  end\n"
    "  return true\n"
    "end\n";

int main(void) {
    cel_lua_t *L = make(HOOKS);
    check("install + load hooks", L != NULL, "");
    if (!L) return 1;

    cJSON *admin  = cJSON_Parse("{\"role\":\"admin\",\"email\":\"a@x\"}");
    cJSON *viewer = cJSON_Parse("{\"role\":\"viewer\",\"email\":\"v@x\"}");
    cJSON *row    = cJSON_Parse("{\"id\":1}");

    /* ---- authorize: additional allow gate ---- */
    check("authorize secrets / viewer -> deny",
          cel_hooks_authorize(L, "get", "secrets", (cel_val_t *)row, (cel_val_t *)viewer) == 0, "");
    check("authorize secrets / admin -> allow",
          cel_hooks_authorize(L, "get", "secrets", (cel_val_t *)row, (cel_val_t *)admin) == 1, "");
    check("authorize notes / viewer -> allow",
          cel_hooks_authorize(L, "get", "notes", (cel_val_t *)row, (cel_val_t *)viewer) == 1, "");

    /* ---- before: accept + in-place mutation ---- */
    char err[256];
    cJSON *in = cJSON_Parse("{\"title\":\"   trim me\",\"scratch\":99}");
    int rc = cel_hooks_before(L, "create", "notes", (cel_val_t *)in, (cel_val_t *)admin, err, sizeof err);
    check("before notes/create -> accept", rc == 0, err);
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(in, "title");
    check("input.title trimmed in place", title && !strcmp(title->valuestring, "trim me"),
          title ? title->valuestring : "(null)");
    check("input.normalized added", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(in, "normalized")), "");
    check("input.scratch removed (nil)", cJSON_GetObjectItemCaseSensitive(in, "scratch") == NULL, "");

    /* ---- before: reject with reason ---- */
    cJSON *empty = cJSON_Parse("{}");
    rc = cel_hooks_before(L, "create", "notes", (cel_val_t *)empty, (cel_val_t *)admin, err, sizeof err);
    check("before missing title -> reject", rc == -1, "");
    check("reject reason surfaced", strstr(err, "title is required") != NULL, err);

    /* ---- before: no rule for this table -> accept untouched ---- */
    cJSON *other = cJSON_Parse("{\"x\":1}");
    rc = cel_hooks_before(L, "create", "other", (cel_val_t *)other, (cel_val_t *)admin, err, sizeof err);
    check("before unmatched table -> accept", rc == 0, err);

    /* ---- absent hooks are no-ops (allow / accept) ---- */
    cel_lua_t *bare = make("-- no hooks defined\n");
    check("absent authorize -> allow",
          cel_hooks_authorize(bare, "get", "secrets", (cel_val_t *)row, (cel_val_t *)viewer) == 1, "");
    check("absent before -> accept",
          cel_hooks_before(bare, "create", "notes", (cel_val_t *)empty, (cel_val_t *)admin, err, sizeof err) == 0, "");

    /* ---- a faulting hook fails closed ---- */
    cel_lua_t *bad = make("function authorize() error('boom') end\n"
                          "function before() error('kaboom') end\n");
    check("faulting authorize -> deny",
          cel_hooks_authorize(bad, "get", "notes", (cel_val_t *)row, (cel_val_t *)admin) == 0, "");
    rc = cel_hooks_before(bad, "create", "notes", (cel_val_t *)empty, (cel_val_t *)admin, err, sizeof err);
    check("faulting before -> reject", rc == -1, "");
    check("fault reason captured", strstr(err, "kaboom") != NULL, err);

    cJSON_Delete(admin); cJSON_Delete(viewer); cJSON_Delete(row);
    cJSON_Delete(in); cJSON_Delete(empty); cJSON_Delete(other);
    cel_lua_close(L); cel_lua_close(bare); cel_lua_close(bad);

    printf("\n%s  (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
