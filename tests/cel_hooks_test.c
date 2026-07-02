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
#include <sqlite3.h>

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
    "end\n"
    "function rpc(name, args, who)\n"
    "  if name == 'ping' then return { ok = true, by = who.email } end\n"
    "  if name == 'count_notes' then\n"
    "    local rows = cellar.query('SELECT count(*) AS n FROM notes WHERE owner_id = ?', { who.user_id })\n"
    "    return { count = rows[1].n }\n"
    "  end\n"
    "  if name == 'add_note' then\n"
    "    cellar.exec('INSERT INTO notes(owner_id, title) VALUES (?, ?)', { who.user_id, args.title })\n"
    "    return { ok = true }\n"
    "  end\n"
    "  if name == 'list_titles' then\n"
    "    local rows = cellar.query('SELECT title FROM notes WHERE owner_id = ? ORDER BY title', { who.user_id })\n"
    "    local out = {}\n"
    "    for i = 1, #rows do out[i] = rows[i].title end\n"
    "    return out\n"   /* a Lua array -> JSON array */
    "  end\n"
    "  if name == 'bad_query' then return cellar.query('SELECT * FROM nope') end\n"
    "  return nil, 'unknown rpc: ' .. name\n"
    "end\n"
    "function on_realtime(change, subscriber)\n"
    "  if change.op == 'DELETE' then return false end          -- never push deletes\n"
    "  if subscriber.role == 'muted' then return false end     -- role-gated\n"
    "  return true\n"
    "end\n"
    "function render_email(kind, ctx)\n"
    "  if kind == 'password_reset' then\n"
    "    return { subject = 'Reset (branded)',\n"
    "             html = '<h1>Reset</h1><a href=\"' .. tostring(ctx.url) .. '\">go</a>',\n"
    "             text = 'Reset link: ' .. tostring(ctx.url) }\n"
    "  end\n"
    "  if kind == 'optout' then return nil end\n"                 /* -> C falls back to built-in */
    "  if kind == 'notatable' then return 42 end\n"               /* non-table -> C falls back */
    "  return { subject = 'Hi ' .. tostring(ctx.email) }\n"       /* subject only, no html */
    "end\n";

int main(void) {
    cel_lua_t *L = make(HOOKS);
    check("install + load hooks", L != NULL, "");
    if (!L) return 1;

    cJSON *admin  = cJSON_Parse("{\"role\":\"admin\",\"email\":\"a@x\",\"user_id\":\"u1\"}");
    cJSON *viewer = cJSON_Parse("{\"role\":\"viewer\",\"email\":\"v@x\",\"user_id\":\"u2\"}");
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

    /* ---- rpc + db side-effects (cellar.query/exec on the request's conn) ---- */
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    sqlite3_exec(db, "CREATE TABLE notes(id INTEGER PRIMARY KEY, owner_id TEXT, title TEXT);"
                     "INSERT INTO notes(owner_id,title) VALUES('u1','alpha'),('u1','beta'),('u2','gamma');",
                 NULL, NULL, NULL);
    cel_hooks_set_db(db);   /* the dispatcher binds this per-request; here we set it directly */

    /* rpc with no db need: returns an object built from `who` */
    cel_val_t *res = cel_hooks_rpc(L, "ping", NULL, (cel_val_t *)admin, err, sizeof err);
    check("rpc ping -> result", res != NULL, err);
    check("rpc result is object {ok,by}", res &&
          cel_val_bool(cel_val_get(res, "ok")) == 1 &&
          cel_val_str(cel_val_get(res, "by")) && !strcmp(cel_val_str(cel_val_get(res, "by")), "a@x"), "");
    cel_val_free(res);

    /* rpc that QUERIES the db (count u1's notes -> 2) */
    res = cel_hooks_rpc(L, "count_notes", NULL, (cel_val_t *)admin, err, sizeof err);
    check("rpc count_notes via cellar.query", res && cel_val_num(cel_val_get(res, "count")) == 2.0,
          res ? "" : err);
    cel_val_free(res);

    /* rpc that WRITES via cellar.exec, then re-count -> 3 */
    cJSON *add_args = cJSON_Parse("{\"title\":\"delta\"}");
    res = cel_hooks_rpc(L, "add_note", (cel_val_t *)add_args, (cel_val_t *)admin, err, sizeof err);
    check("rpc add_note via cellar.exec", res && cel_val_bool(cel_val_get(res, "ok")) == 1, res ? "" : err);
    cel_val_free(res);
    res = cel_hooks_rpc(L, "count_notes", NULL, (cel_val_t *)admin, err, sizeof err);
    check("count reflects the insert -> 3", res && cel_val_num(cel_val_get(res, "count")) == 3.0, "");
    cel_val_free(res);

    /* rpc returning a Lua ARRAY -> JSON array (marshal a sequence) */
    res = cel_hooks_rpc(L, "list_titles", NULL, (cel_val_t *)admin, err, sizeof err);
    check("rpc list_titles -> array", res && cel_val_type(res) == CEL_V_ARR && cel_val_len(res) == 3, "");
    check("array element marshaled", res && cel_val_str(cel_val_at(res, 0)) &&
          !strcmp(cel_val_str(cel_val_at(res, 0)), "alpha"), "");
    cel_val_free(res);

    /* db isolation by binding: u2 sees only its 1 note */
    res = cel_hooks_rpc(L, "count_notes", NULL, (cel_val_t *)viewer, err, sizeof err);
    check("rpc honors who (u2 -> 1)", res && cel_val_num(cel_val_get(res, "count")) == 1.0, "");
    cel_val_free(res);

    /* unknown rpc -> NULL + reason */
    res = cel_hooks_rpc(L, "nope", NULL, (cel_val_t *)admin, err, sizeof err);
    check("unknown rpc -> NULL", res == NULL, "");
    check("unknown rpc reason", strstr(err, "unknown rpc") != NULL, err);

    /* a db error inside an rpc -> fault -> NULL + reason (not a crash) */
    res = cel_hooks_rpc(L, "bad_query", NULL, (cel_val_t *)admin, err, sizeof err);
    check("rpc with bad query -> NULL", res == NULL, "");
    check("bad query reason surfaced", err[0] != '\0', err);

    cel_hooks_set_db(NULL);
    sqlite3_close(db);

    /* ---- on_realtime: per-subscriber delivery filter ---- */
    cJSON *chg_ins = cJSON_Parse("{\"table\":\"notes\",\"op\":\"INSERT\",\"row\":{\"id\":1}}");
    cJSON *chg_del = cJSON_Parse("{\"table\":\"notes\",\"op\":\"DELETE\",\"row\":{\"id\":1}}");
    cJSON *sub_norm = cJSON_Parse("{\"user_id\":\"u1\",\"role\":\"viewer\"}");
    cJSON *sub_muted = cJSON_Parse("{\"user_id\":\"u9\",\"role\":\"muted\"}");
    check("on_realtime deliver INSERT to normal sub",
          cel_hooks_on_realtime(L, (cel_val_t *)chg_ins, (cel_val_t *)sub_norm) == 1, "");
    check("on_realtime drops DELETE",
          cel_hooks_on_realtime(L, (cel_val_t *)chg_del, (cel_val_t *)sub_norm) == 0, "");
    check("on_realtime drops muted subscriber",
          cel_hooks_on_realtime(L, (cel_val_t *)chg_ins, (cel_val_t *)sub_muted) == 0, "");
    check("on_realtime absent -> deliver",
          cel_hooks_on_realtime(bare, (cel_val_t *)chg_ins, (cel_val_t *)sub_norm) == 1, "");
    cJSON_Delete(chg_ins); cJSON_Delete(chg_del); cJSON_Delete(sub_norm); cJSON_Delete(sub_muted);

    /* ---- render_email hook (per-app branded emails) ---- */
    cJSON *em_ctx = cJSON_Parse("{\"url\":\"https://x/reset?t=abc\",\"email\":\"u@x\"}");
    cel_val_t *e1 = cel_hooks_render_email(L, "password_reset", (cel_val_t *)em_ctx);
    check("render_email returns table", e1 != NULL, "");
    check("render_email subject override",
          e1 && cel_val_str(cel_val_get(e1, "subject")) &&
          strcmp(cel_val_str(cel_val_get(e1, "subject")), "Reset (branded)") == 0, "");
    check("render_email html carries ctx.url",
          e1 && cel_val_str(cel_val_get(e1, "html")) &&
          strstr(cel_val_str(cel_val_get(e1, "html")), "reset?t=abc") != NULL, "");
    check("render_email text present", e1 && cel_val_str(cel_val_get(e1, "text")) != NULL, "");
    cel_val_free(e1);

    cel_val_t *e2 = cel_hooks_render_email(L, "optout", (cel_val_t *)em_ctx);
    check("render_email nil return -> NULL (fall back)", e2 == NULL, "");
    cel_val_free(e2);
    cel_val_t *e3 = cel_hooks_render_email(L, "notatable", (cel_val_t *)em_ctx);
    check("render_email non-table -> NULL (fall back)", e3 == NULL, "");
    cel_val_free(e3);
    cel_val_t *e4 = cel_hooks_render_email(L, "verify_email", (cel_val_t *)em_ctx);
    check("render_email subject-only (no html)",
          e4 && cel_val_str(cel_val_get(e4, "subject")) &&
          cel_val_str(cel_val_get(e4, "html")) == NULL, "");
    cel_val_free(e4);
    cel_val_t *e5 = cel_hooks_render_email(bare, "password_reset", (cel_val_t *)em_ctx);
    check("render_email absent hook -> NULL (fall back)", e5 == NULL, "");
    cel_val_free(e5);
    cJSON_Delete(em_ctx);

    cJSON_Delete(admin); cJSON_Delete(viewer); cJSON_Delete(row);
    cJSON_Delete(in); cJSON_Delete(empty); cJSON_Delete(other); cJSON_Delete(add_args);
    cel_lua_close(L); cel_lua_close(bare); cel_lua_close(bad);

    printf("\n%s  (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
