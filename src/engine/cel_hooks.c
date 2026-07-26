/* cellar — the Lua hook dispatcher (design §8). See cel_hooks.h. */
#include "cel_hooks.h"
#include "cel_apps.h"
#include "realtime.h"
#include "policy.h"
#include "core/notif_channel.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reserved JobQueue type: cellar.notify enqueues this; cel_hooks_run_jobs handles
 * it in engine C (NotifChannel fan-out) instead of the bundle's Lua `job` hook. */
#define CEL_NOTIF_JOB_TYPE "cel:notif"

#include <sqlite3.h>
#include <cjson/cJSON.h>

#include "lua.h"
#include "lauxlib.h"

/* The connection this thread's hook db side-effects run against, set by the
 * dispatcher (or a test) around a hook call to the request's connection. */
static __thread sqlite3 *t_db = NULL;
void cel_hooks_set_db(void *conn) { t_db = (sqlite3 *)conn; }

/* ---- side-effect C API (called from Lua via ffi.C) ------------------------- */
void cel_hook_log(int level, const char *msg) {
    if (!msg) msg = "";
    switch (level) {
        case 0:  LOG_DEBUG("[hook] %s", msg); break;
        case 2:  LOG_WARN ("[hook] %s", msg); break;
        case 3:  LOG_ERROR("[hook] %s", msg); break;
        default: LOG_INFO ("[hook] %s", msg); break;
    }
}

/* Bind a cel_val array of scalars to a prepared statement, by position. */
static void bind_params(sqlite3_stmt *st, const cel_val_t *params) {
    int n = cel_val_len(params);
    for (int i = 0; i < n; i++) {
        const cel_val_t *p = cel_val_at(params, i);
        switch (cel_val_type(p)) {
            case CEL_V_STR:  sqlite3_bind_text(st, i + 1, cel_val_str(p), -1, SQLITE_TRANSIENT); break;
            case CEL_V_BOOL: sqlite3_bind_int (st, i + 1, cel_val_bool(p)); break;
            case CEL_V_NUM: {
                double d = cel_val_num(p);
                if (d == (double)(long long)d) sqlite3_bind_int64(st, i + 1, (long long)d);
                else                           sqlite3_bind_double(st, i + 1, d);
                break;
            }
            default: sqlite3_bind_null(st, i + 1); break;
        }
    }
}

/* Serialize a stepped result set to a cJSON array of row objects, typed by the
 * runtime storage class (hooks are first-party; raw column types are fine). */
static cJSON *rows_to_json(sqlite3_stmt *st) {
    cJSON *arr = cJSON_CreateArray();
    while (sqlite3_step(st) == SQLITE_ROW) {
        cJSON *row = cJSON_CreateObject();
        int nc = sqlite3_column_count(st);
        for (int c = 0; c < nc; c++) {
            const char *name = sqlite3_column_name(st, c);
            switch (sqlite3_column_type(st, c)) {
                case SQLITE_INTEGER: cJSON_AddNumberToObject(row, name, (double)sqlite3_column_int64(st, c)); break;
                case SQLITE_FLOAT:   cJSON_AddNumberToObject(row, name, sqlite3_column_double(st, c)); break;
                case SQLITE_TEXT:    cJSON_AddStringToObject(row, name, (const char *)sqlite3_column_text(st, c)); break;
                default:             cJSON_AddNullToObject(row, name); break;   /* NULL / BLOB */
            }
        }
        cJSON_AddItemToArray(arr, row);
    }
    return arr;
}

cel_val_t *cel_hook_query(const char *sql, const cel_val_t *params, char *err, int errlen) {
    if (err && errlen) err[0] = '\0';
    if (!t_db) { if (err && errlen) snprintf(err, errlen, "no database in this context"); return NULL; }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t_db, sql ? sql : "", -1, &st, NULL) != SQLITE_OK) {
        if (err && errlen) snprintf(err, errlen, "%s", sqlite3_errmsg(t_db));
        return NULL;
    }
    bind_params(st, params);
    cJSON *arr = rows_to_json(st);
    if (sqlite3_finalize(st) != SQLITE_OK) {
        if (err && errlen) snprintf(err, errlen, "%s", sqlite3_errmsg(t_db));
        cJSON_Delete(arr);
        return NULL;
    }
    return (cel_val_t *)arr;
}

long long cel_hook_exec(const char *sql, const cel_val_t *params, char *err, int errlen) {
    if (err && errlen) err[0] = '\0';
    if (!t_db) { if (err && errlen) snprintf(err, errlen, "no database in this context"); return -1; }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t_db, sql ? sql : "", -1, &st, NULL) != SQLITE_OK) {
        if (err && errlen) snprintf(err, errlen, "%s", sqlite3_errmsg(t_db));
        return -1;
    }
    bind_params(st, params);
    while (sqlite3_step(st) == SQLITE_ROW) { }   /* run to completion */
    if (sqlite3_finalize(st) != SQLITE_OK) {
        if (err && errlen) snprintf(err, errlen, "%s", sqlite3_errmsg(t_db));
        return -1;
    }
    return (long long)sqlite3_changes(t_db);
}

/* EventSink: append an event onto the current app's log (its own connection,
 * independent of the request txn). Best-effort — telemetry never fails a hook. */
void cel_hook_emit(const char *type, const char *actor, const char *subject, const char *props) {
    event_sink_t *s = cel_apps_current_events();
    if (!s || !type || !type[0]) return;
    event_t ev = {
        .type       = type,
        .actor_id   = (actor   && actor[0])   ? actor   : NULL,
        .subject_id = (subject && subject[0]) ? subject : NULL,
        .props      = (props   && props[0])   ? props   : NULL,
    };
    s->emit(s->ctx, &ev);
}

/* JobQueue: enqueue onto the current app's queue. Returns the new id, or -1. */
long long cel_hook_enqueue(const char *type, const char *payload,
                           long long run_at, long long repeat_every) {
    job_queue_t *q = cel_apps_current_jobs();
    if (!q || !type || !type[0]) return -1;
    long long id = -1;
    q->enqueue(q->ctx, type, (payload && payload[0]) ? payload : NULL,
               run_at, 0, repeat_every, &id);
    return id;
}

/* NotifChannel: build the {user_id, msg:{title,body,url,data}} payload and enqueue
 * the reserved cel:notif job; cel_hooks_run_jobs fans it out in C. Returns id or -1. */
long long cel_hook_notify(const char *user_id, const char *title, const char *body,
                          const char *url, const char *data_json) {
    job_queue_t *q = cel_apps_current_jobs();
    if (!q || !user_id || !user_id[0]) return -1;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "user_id", user_id);
    cJSON *m = cJSON_AddObjectToObject(root, "msg");
    if (title && title[0]) cJSON_AddStringToObject(m, "title", title);
    if (body  && body[0])  cJSON_AddStringToObject(m, "body",  body);
    if (url   && url[0])   cJSON_AddStringToObject(m, "url",   url);
    if (data_json && data_json[0]) cJSON_AddStringToObject(m, "data", data_json);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return -1;
    long long id = -1;
    q->enqueue(q->ctx, CEL_NOTIF_JOB_TYPE, payload, 0, 0, 0, &id);
    free(payload);
    return id;
}

/* Fan-out for a cel:notif job: resolve the user's channels off the bound connection
 * (t_db) and deliver via the registered NotifChannel adapters. Per-channel failures
 * are logged + skipped. Email is resolved here; webpush iterates the user's active
 * subscriptions — that adapter is registered in slice 2, so the loop is inert (skipped)
 * until then. */
/* Returns 0 on success / nothing-to-do, or -1 if a channel send was attempted and
 * failed transiently (the caller retries the job with backoff). Only READS the db
 * (resolve recipients) — the caller drops the app write lock around it, so the
 * blocking channel send must not write. */
static int cel_notif_fanout(cel_lua_t *Lh, const char *payload) {
    if (!t_db || !payload || !payload[0]) return 0;
    cJSON *root = cJSON_Parse(payload);
    if (!root) return 0;
    const char *user_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "user_id"));
    cJSON *m = cJSON_GetObjectItem(root, "msg");
    if (!user_id || !user_id[0] || !cJSON_IsObject(m)) { cJSON_Delete(root); return 0; }
    cel_notif_msg_t msg = {
        .title     = cJSON_GetStringValue(cJSON_GetObjectItem(m, "title")),
        .body      = cJSON_GetStringValue(cJSON_GetObjectItem(m, "body")),
        .url       = cJSON_GetStringValue(cJSON_GetObjectItem(m, "url")),
        .data_json = cJSON_GetStringValue(cJSON_GetObjectItem(m, "data")),
    };
    /* Optional per-app render_email("notification", ctx=msg): may override the
     * subject/text and add an HTML body (email channel uses it; push ignores html).
     * t_db is bound here, so the hook may cellar.query. `rendered` outlives the send. */
    cel_val_t *rendered = Lh ? cel_hooks_render_email(Lh, "notification", (const cel_val_t *)m) : NULL;
    if (rendered) {
        const char *s = cel_val_str(cel_val_get(rendered, "subject"));
        const char *t = cel_val_str(cel_val_get(rendered, "text"));
        const char *h = cel_val_str(cel_val_get(rendered, "html"));
        if (s && *s) msg.title = s;
        if (t && *t) msg.body  = t;
        if (h && *h) msg.html  = h;
    }
    char err[256];
    int rc = 0;   /* -1 → a transient channel failure happened; retry the job */

    const cel_notif_channel_t *email = cel_notif_get("email");
    if (email) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(t_db, "SELECT email FROM cel_users WHERE id=?1 AND is_active=1",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) {
                const char *to = (const char *)sqlite3_column_text(st, 0);
                if (to && to[0]) {
                    err[0] = '\0';
                    if (email->send(to, &msg, err, sizeof err) < 0) {
                        LOG_WARN("[notif] email send failed: %s", err);
                        rc = -1;   /* transient (SMTP) — let the job retry */
                    }
                }
            }
            sqlite3_finalize(st);
        }
    }
    /* webpush: slice 2 registers the adapter + sends per subscription (+ prunes on GONE). */
    cel_val_free(rendered);
    cJSON_Delete(root);
    return rc;
}

/* Realtime: publish a change for a server-created row (e.g. a notification) so
 * subscribers get it live — same fan-out the CRUD write path uses (rt_emit), but
 * reachable from a hook/job that inserted the row via raw SQL. `row_json` is the
 * row object; delivery is scoped by the table's policy (owner/VIA) + on_realtime. */
void cel_hook_rt_emit(const char *table, const char *op, const char *row_json) {
    if (!table || !op || !row_json) return;
    if (!cel_realtime_active() || !cel_policy_realtime_enabled(table)) return;
    cJSON *row = cJSON_Parse(row_json);
    if (!cJSON_IsObject(row)) { cJSON_Delete(row); return; }
    /* publish drives the on_realtime filter on THIS thread's Lua state, reentrantly
     * inside the current hook/job pcall. On the worker job path t_db is bound to
     * the job's connection; the on_realtime contract is "no db in this context"
     * (and re-entering the same connection here would be unsafe). Clear t_db across
     * the publish so on_realtime behaves identically on the request and worker
     * paths, then restore it for the rest of the job. */
    sqlite3 *saved = t_db;
    t_db = NULL;
    cel_realtime_publish((const void *)app_db_current(), table, op, row);
    t_db = saved;
    cJSON_Delete(row);
}

/* Account creation is wired by the engine at boot (cel_hooks_set_user_creator),
 * so this file stays free of the auth layer (and its libsodium/db deps) — the
 * same decoupling as the realtime filter. NULL until wired (or in unit tests). */
static cel_hook_create_user_fn g_create_user = NULL;
void cel_hooks_set_user_creator(cel_hook_create_user_fn fn) { g_create_user = fn; }

/* Create a login account (a 'password' identity) in the current app, exposed to
 * hooks as cellar.create_user. The bundle decides WHO may call it and for which
 * target role (e.g. an rpc that lets a manager mint clerks only) — the engine just
 * provides the primitive. Refuses platform_admin (the out-of-band escalation
 * boundary, mirroring POST /auth/users). Returns 0 + the new user id, or -1 + err. */
int cel_hook_create_user(const char *email, const char *password, const char *role,
                         char *out_id, int out_id_size, char *err, int errlen) {
    if (err && errlen) err[0] = '\0';
    if (!email || !email[0] || !password || !password[0] || !role || !role[0]) {
        if (err && errlen) snprintf(err, errlen, "email, password and role are required");
        return -1;
    }
    /* Refuse ANY superuser role — the engine treats 'admin' and any superuser:true
     * config role as a full superuser (policy.c), not just 'platform_admin'. The
     * /rpc path has no caller authz, so this engine-side guard is the floor that
     * stops a hook minting a privileged account. */
    if (cel_role_is_superuser(role)) {
        if (err && errlen) snprintf(err, errlen, "cannot create a superuser role ('%s') from a hook", role);
        return -1;
    }
    /* Password policy (mirror cel_api_create_user; the primitive must not be a
     * weaker creation path). */
    size_t plen = strlen(password);
    if (plen < 8)   { if (err && errlen) snprintf(err, errlen, "password too short (min 8 characters)"); return -1; }
    if (plen > 128) { if (err && errlen) snprintf(err, errlen, "password too long (max 128 characters)"); return -1; }
    /* cel_auth_create_user takes the app's non-recursive write lock. before()/
     * after()/resolve()/job hooks ALREADY run under it, so re-locking would
     * self-deadlock the worker. Only the rpc path (no write lock held) is safe. */
    if (app_db_in_write_lock()) {
        if (err && errlen) snprintf(err, errlen, "create_user is only available from an rpc hook (not before/after/resolve/job)");
        return -1;
    }
    if (!g_create_user) {
        if (err && errlen) snprintf(err, errlen, "user creation is not available in this context");
        return -1;
    }
    return g_create_user(email, password, role, out_id, out_id_size, err, errlen);
}

/* Password reset for an existing account is wired at boot (cel_hooks_set_password_setter). */
static cel_hook_set_password_fn g_set_password = NULL;
void cel_hooks_set_password_setter(cel_hook_set_password_fn fn) { g_set_password = fn; }

/* Set (reset) an existing account's password, exposed to hooks as cellar.set_password.
 * Like create_user, the bundle's rpc decides WHO may reset WHOM (it can read the
 * target role via cellar.query); the engine provides the primitive — the one thing
 * Lua can't do (argon2id-hash + set a secret) — and the wired adapter refuses
 * resetting a superuser. Returns 0, or -1 + err. */
int cel_hook_set_password(const char *email, const char *new_password, char *err, int errlen) {
    if (err && errlen) err[0] = '\0';
    if (!email || !email[0] || !new_password || !new_password[0]) {
        if (err && errlen) snprintf(err, errlen, "email and new_password are required");
        return -1;
    }
    /* Same password policy as create_user / POST /auth/password/reset — the
     * primitive must not be a weaker path. */
    size_t plen = strlen(new_password);
    if (plen < 8)   { if (err && errlen) snprintf(err, errlen, "password too short (min 8 characters)"); return -1; }
    if (plen > 128) { if (err && errlen) snprintf(err, errlen, "password too long (max 128 characters)"); return -1; }
    /* cel_auth_set_password takes the app's non-recursive write lock; before()/
     * after()/resolve()/job hooks already hold it, so re-locking would self-deadlock
     * the worker. Only the rpc path (no write lock held) is safe. */
    if (app_db_in_write_lock()) {
        if (err && errlen) snprintf(err, errlen, "set_password is only available from an rpc hook (not before/after/resolve/job)");
        return -1;
    }
    if (!g_set_password) {
        if (err && errlen) snprintf(err, errlen, "password reset is not available in this context");
        return -1;
    }
    return g_set_password(email, new_password, err, errlen);
}

/* Enable/disable + hard-delete an account, exposed as cellar.set_user_active /
 * cellar.delete_user. Same shape as set_password: the bundle rpc decides WHO may
 * act on WHOM (read the target role via cellar.query); the wired adapters refuse a
 * superuser and provide the primitives (flip is_active + revoke, or DELETE + cascade). */
static cel_hook_set_active_fn  g_set_active  = NULL;
void cel_hooks_set_user_activator(cel_hook_set_active_fn fn) { g_set_active = fn; }
static cel_hook_delete_user_fn g_delete_user = NULL;
void cel_hooks_set_user_deleter(cel_hook_delete_user_fn fn) { g_delete_user = fn; }

int cel_hook_set_user_active(const char *email, int active, char *err, int errlen) {
    if (err && errlen) err[0] = '\0';
    if (!email || !email[0]) { if (err && errlen) snprintf(err, errlen, "email is required"); return -1; }
    /* Takes the app write lock (cel_auth_set_active); before/after/resolve/job
     * already hold it, so re-locking would self-deadlock — rpc-only. */
    if (app_db_in_write_lock()) {
        if (err && errlen) snprintf(err, errlen, "set_user_active is only available from an rpc hook (not before/after/resolve/job)");
        return -1;
    }
    if (!g_set_active) {
        if (err && errlen) snprintf(err, errlen, "account status change is not available in this context");
        return -1;
    }
    return g_set_active(email, active, err, errlen);
}

int cel_hook_delete_user(const char *email, char *err, int errlen) {
    if (err && errlen) err[0] = '\0';
    if (!email || !email[0]) { if (err && errlen) snprintf(err, errlen, "email is required"); return -1; }
    if (app_db_in_write_lock()) {
        if (err && errlen) snprintf(err, errlen, "delete_user is only available from an rpc hook (not before/after/resolve/job)");
        return -1;
    }
    if (!g_delete_user) {
        if (err && errlen) snprintf(err, errlen, "account deletion is not available in this context");
        return -1;
    }
    return g_delete_user(email, err, errlen);
}

/* ---- the prelude: FFI cdef + `cellar` sugar + per-hook trampolines ---------
 * Loaded into every state before its hooks.lua. The trampolines box the raw
 * cel_val_t* (passed from C as a lightuserdata) into table-like proxies, call the
 * user's global hook if defined, and normalize the return for the C dispatcher. */
static const char *PRELUDE =
"local ffi = require('ffi')\n"
"ffi.cdef[[\n"
"  typedef struct cel_val cel_val_t;\n"
"  int              cel_val_type(const cel_val_t*);\n"
"  const cel_val_t* cel_val_get (const cel_val_t*, const char*);\n"
"  const cel_val_t* cel_val_at  (const cel_val_t*, int);\n"
"  int              cel_val_len (const cel_val_t*);\n"
"  const char*      cel_val_key (const cel_val_t*, int);\n"
"  const char*      cel_val_str (const cel_val_t*);\n"
"  double           cel_val_num (const cel_val_t*);\n"
"  int              cel_val_bool(const cel_val_t*);\n"
"  void cel_val_set_str (cel_val_t*, const char*, const char*);\n"
"  void cel_val_set_num (cel_val_t*, const char*, double);\n"
"  void cel_val_set_bool(cel_val_t*, const char*, int);\n"
"  void cel_val_set_null(cel_val_t*, const char*);\n"
"  void cel_val_unset   (cel_val_t*, const char*);\n"
"  cel_val_t* cel_val_new_array(void);\n"
"  void cel_val_push_str (cel_val_t*, const char*);\n"
"  void cel_val_push_num (cel_val_t*, double);\n"
"  void cel_val_push_bool(cel_val_t*, int);\n"
"  void cel_val_push_null(cel_val_t*);\n"
"  void cel_val_free     (cel_val_t*);\n"
"  void       cel_hook_log  (int, const char*);\n"
"  cel_val_t* cel_hook_query(const char*, const cel_val_t*, char*, int);\n"
"  long long  cel_hook_exec (const char*, const cel_val_t*, char*, int);\n"
"  void       cel_hook_emit (const char*, const char*, const char*, const char*);\n"
"  long long  cel_hook_enqueue(const char*, const char*, long long, long long);\n"
"  void       cel_hook_rt_emit(const char*, const char*, const char*);\n"
"  long long  cel_hook_notify(const char*, const char*, const char*, const char*, const char*);\n"
"  int        cel_hook_create_user(const char*, const char*, const char*, char*, int, char*, int);\n"
"  int        cel_hook_set_password(const char*, const char*, char*, int);\n"
"  int        cel_hook_set_user_active(const char*, int, char*, int);\n"
"  int        cel_hook_delete_user(const char*, char*, int);\n"
"]]\n"
"local C = ffi.C\n"
"local NUL, BOOL, NUM, STR, OBJ, ARR = 0,1,2,3,4,5\n"
"\n"
"local box  -- fwd decl (proxies wrap container handles)\n"
"local function to_lua(cv)\n"
"  if cv == nil then return nil end\n"
"  local t = C.cel_val_type(cv)\n"
"  if     t == NUL  then return nil\n"
"  elseif t == BOOL then return C.cel_val_bool(cv) ~= 0\n"
"  elseif t == NUM  then return tonumber(C.cel_val_num(cv))\n"
"  elseif t == STR  then return ffi.string(C.cel_val_str(cv))\n"
"  else                  return box(cv, false) end  -- OBJ/ARR stay live handles\n"
"end\n"
"\n"
"local proxy = {}\n"
"proxy.__index = function(self, k)\n"
"  local cv = rawget(self, '_cv')\n"
"  if type(k) == 'number' then return to_lua(C.cel_val_at(cv, k - 1)) end  -- 1-based\n"
"  return to_lua(C.cel_val_get(cv, k))\n"
"end\n"
"proxy.__newindex = function(self, k, v)\n"
"  if not rawget(self, '_w') then error('value is read-only', 2) end\n"
"  if type(k) ~= 'string' then error('hook can only set string keys', 2) end\n"
"  local cv, tv = rawget(self, '_cv'), type(v)\n"
"  if     v  == nil      then C.cel_val_unset(cv, k)            -- nil removes\n"
"  elseif tv == 'string'  then C.cel_val_set_str (cv, k, v)\n"
"  elseif tv == 'number'  then C.cel_val_set_num (cv, k, v)\n"
"  elseif tv == 'boolean' then C.cel_val_set_bool(cv, k, v and 1 or 0)\n"
"  else error('unsupported value type for hook input: ' .. tv, 2) end\n"
"end\n"
"proxy.__len = function(self) return C.cel_val_len(rawget(self, '_cv')) end\n"
"box = function(ptr, writable)\n"
"  return setmetatable({ _cv = ffi.cast('cel_val_t*', ptr), _w = writable and true or false }, proxy)\n"
"end\n"
"\n"
"cellar = {\n"
"  log = {\n"
"    debug = function(m) C.cel_hook_log(0, tostring(m)) end,\n"
"    info  = function(m) C.cel_hook_log(1, tostring(m)) end,\n"
"    warn  = function(m) C.cel_hook_log(2, tostring(m)) end,\n"
"    error = function(m) C.cel_hook_log(3, tostring(m)) end,\n"
"  },\n"
"}\n"
"-- EventSink: cellar.emit(type, {actor=, subject=, props=}) — fire-and-forget\n"
"-- onto this app's event log (props is a JSON string). Best-effort.\n"
"function cellar.emit(etype, o)\n"
"  o = o or {}\n"
"  C.cel_hook_emit(tostring(etype), o.actor or '', o.subject or '', o.props or '')\n"
"end\n"
"-- JobQueue: cellar.enqueue_job(type, payload, {run_at=, repeat_every=}) →\n"
"-- the new job id (or -1). payload is a JSON string.\n"
"function cellar.enqueue_job(jtype, payload, o)\n"
"  o = o or {}\n"
"  return tonumber(C.cel_hook_enqueue(tostring(jtype), payload or '',\n"
"                                     o.run_at or 0, o.repeat_every or 0))\n"
"end\n"
"-- Realtime: push a server-created row (e.g. a notification) to live subscribers.\n"
"function cellar.rt_emit(tbl, op, row_json)\n"
"  C.cel_hook_rt_emit(tostring(tbl), tostring(op), tostring(row_json))\n"
"end\n"
"-- Off-site notification: cellar.notify(user_id, {title=,body=,url=,data=}) -> bool.\n"
"-- Enqueues a fan-out job; the engine delivers to the user's channels (email/push).\n"
"-- `data` is an optional JSON string. This is the OFF-SITE path — pair it with your\n"
"-- own in-app feed insert + rt_emit. Returns true if enqueued.\n"
"function cellar.notify(user_id, msg)\n"
"  msg = msg or {}\n"
"  local id = C.cel_hook_notify(tostring(user_id), tostring(msg.title or ''),\n"
"    tostring(msg.body or ''), tostring(msg.url or ''), tostring(msg.data or ''))\n"
"  return tonumber(id) >= 0\n"
"end\n"
"-- Account creation: cellar.create_user(email, password, role) -> id, or nil+err.\n"
"-- The bundle's rpc decides who may call this and which role; the engine refuses\n"
"-- platform_admin. Pair it with an INSERT into your own roster table if needed.\n"
"function cellar.create_user(email, password, role)\n"
"  local idbuf, errbuf = ffi.new('char[64]'), ffi.new('char[256]')\n"
"  local rc = C.cel_hook_create_user(tostring(email), tostring(password), tostring(role), idbuf, 64, errbuf, 256)\n"
"  if rc ~= 0 then return nil, ffi.string(errbuf) end\n"
"  return ffi.string(idbuf)\n"
"end\n"
"-- Password reset: cellar.set_password(email, new_password) -> true, or nil+err.\n"
"-- Set an existing account's password with no current-password / email round-trip.\n"
"-- The bundle's rpc decides who may reset whom (read the target role via\n"
"-- cellar.query); the engine refuses resetting a superuser. Revokes the target's\n"
"-- sessions + MFA on success.\n"
"function cellar.set_password(email, new_password)\n"
"  local errbuf = ffi.new('char[256]')\n"
"  local rc = C.cel_hook_set_password(tostring(email), tostring(new_password), errbuf, 256)\n"
"  if rc ~= 0 then return nil, ffi.string(errbuf) end\n"
"  return true\n"
"end\n"
"-- Enable/disable an account: cellar.set_user_active(email, active) -> true, or nil+err.\n"
"-- Disabling drops the target's sessions + device tokens (login stops immediately);\n"
"-- the bundle's rpc decides who may act on whom; the engine refuses a superuser.\n"
"function cellar.set_user_active(email, active)\n"
"  local errbuf = ffi.new('char[256]')\n"
"  local rc = C.cel_hook_set_user_active(tostring(email), (active and active ~= 0) and 1 or 0, errbuf, 256)\n"
"  if rc ~= 0 then return nil, ffi.string(errbuf) end\n"
"  return true\n"
"end\n"
"-- Hard-delete an account: cellar.delete_user(email) -> true, or nil+err. Removes the\n"
"-- cel_users row + all cel_* children (FK cascade); frees the email for reuse. Pair it\n"
"-- with removing your own roster row. The engine refuses deleting a superuser.\n"
"function cellar.delete_user(email)\n"
"  local errbuf = ffi.new('char[256]')\n"
"  local rc = C.cel_hook_delete_user(tostring(email), errbuf, 256)\n"
"  if rc ~= 0 then return nil, ffi.string(errbuf) end\n"
"  return true\n"
"end\n"
"\n"
"-- deep copy a result handle into plain Lua values (the handle is C-owned and\n"
"-- freed right after, so rows can't be live proxies)\n"
"local function deep(cv)\n"
"  if cv == nil then return nil end\n"
"  local t = C.cel_val_type(cv)\n"
"  if     t == NUL  then return nil\n"
"  elseif t == BOOL then return C.cel_val_bool(cv) ~= 0\n"
"  elseif t == NUM  then return tonumber(C.cel_val_num(cv))\n"
"  elseif t == STR  then return ffi.string(C.cel_val_str(cv))\n"
"  elseif t == ARR  then\n"
"    local n, a = C.cel_val_len(cv), {}\n"
"    for i = 0, n - 1 do a[i + 1] = deep(C.cel_val_at(cv, i)) end\n"
"    return a\n"
"  else\n"
"    local n, o = C.cel_val_len(cv), {}\n"
"    for i = 0, n - 1 do local k = ffi.string(C.cel_val_key(cv, i)); o[k] = deep(C.cel_val_get(cv, k)) end\n"
"    return o\n"
"  end\n"
"end\n"
"\n"
"-- marshal a Lua array of scalar binds into an owned cel_val array handle\n"
"local function build_params(params)\n"
"  local arr = C.cel_val_new_array()\n"
"  if params ~= nil then\n"
"    for i = 1, #params do\n"
"      local v, tv = params[i], type(params[i])\n"
"      if     tv == 'string'  then C.cel_val_push_str (arr, v)\n"
"      elseif tv == 'number'  then C.cel_val_push_num (arr, v)\n"
"      elseif tv == 'boolean' then C.cel_val_push_bool(arr, v and 1 or 0)\n"
"      elseif v  == nil       then C.cel_val_push_null(arr)\n"
"      else C.cel_val_free(arr); error('unsupported bind param type: ' .. tv, 3) end\n"
"    end\n"
"  end\n"
"  return arr\n"
"end\n"
"\n"
"function cellar.query(sql, params)\n"
"  local arr = build_params(params)\n"
"  local err = ffi.new('char[256]')\n"
"  local res = C.cel_hook_query(sql, arr, err, 256)\n"
"  C.cel_val_free(arr)\n"
"  if res == nil then error('query: ' .. ffi.string(err), 2) end\n"
"  local ok, out = pcall(deep, res)\n"   /* free the C result even if the deep-copy throws */
"  C.cel_val_free(res)\n"
"  if not ok then error(out, 2) end\n"
"  return out\n"
"end\n"
"\n"
"function cellar.exec(sql, params)\n"
"  local arr = build_params(params)\n"
"  local err = ffi.new('char[256]')\n"
"  local n = tonumber(C.cel_hook_exec(sql, arr, err, 256))\n"
"  C.cel_val_free(arr)\n"
"  if n < 0 then error('exec: ' .. ffi.string(err), 2) end\n"
"  return n\n"
"end\n"
"\n"
"-- trampolines (called from C; user hooks are the globals authorize/before/rpc)\n"
"function __cel_authorize(op, tbl, row_ptr, who_ptr)\n"
"  if type(authorize) ~= 'function' then return true end\n"
"  return authorize(op, tbl, box(row_ptr, false), box(who_ptr, false)) and true or false\n"
"end\n"
"function __cel_before(op, tbl, input_ptr, who_ptr)\n"
"  if type(before) ~= 'function' then return true end\n"
"  local ok, reason = before(op, tbl, box(input_ptr, true), box(who_ptr, false))\n"
"  if ok == false then return false, reason and tostring(reason) or 'rejected' end\n"
"  return true\n"
"end\n"
"function __cel_after(op, tbl, row_ptr, who_ptr)\n"
"  if type(after) == 'function' then after(op, tbl, box(row_ptr, false), box(who_ptr, false)) end\n"
"end\n"
"function __cel_on_realtime(change_ptr, sub_ptr)\n"
"  if type(on_realtime) ~= 'function' then return true end\n"
"  return on_realtime(box(change_ptr, false), box(sub_ptr, false)) and true or false\n"
"end\n"
"function __cel_rpc(name, args_ptr, who_ptr)\n"
"  if type(rpc) ~= 'function' then return nil, 'no rpc handler for ' .. tostring(name) end\n"
"  local res, errm = rpc(name, box(args_ptr, false), box(who_ptr, false))\n"
"  if res == nil then return nil, errm and tostring(errm) or 'rpc returned nil' end\n"
"  return res\n"   /* a Lua value; the C side marshals it back to JSON */
"end\n"
"function __cel_job(jtype, payload)\n"   /* called by cel_hooks_run_jobs per claimed job */
"  if type(job) ~= 'function' then return true end\n"   /* no handler → drop (complete) */
"  local ok, err = pcall(job, jtype, payload)\n"
"  if ok then return true end\n"
"  return false, tostring(err)\n"   /* error → fail (retry/dead-letter) */
"end\n"
"function __cel_resolve(tbl, incoming_ptr, current_ptr, who_ptr)\n"
"  if type(resolve) ~= 'function' then return 'incoming' end\n"   /* default LWW */
"  local incoming = incoming_ptr ~= nil and box(incoming_ptr, false) or nil\n"
"  local r = resolve(tbl, incoming, box(current_ptr, false), box(who_ptr, false))\n"
"  if r == true  or r == 'incoming' then return 'incoming' end\n"
"  if r == false or r == nil or r == 'current' or r == 'server' then return 'current' end\n"
   /* a merged-row (table) return is NOT yet supported — error loudly rather than
    * silently applying the raw incoming row (which would discard the merge). */
"  error(\"resolve() must return 'incoming' or 'current' (merged-row return not yet supported)\")\n"
"end\n"
"function __cel_render_email(kind, ctx_ptr)\n"   /* per-app branded email bodies (opt-in) */
"  if type(render_email) ~= 'function' then return nil end\n"
"  local res = render_email(kind, box(ctx_ptr, false))\n"
"  if type(res) ~= 'table' then return nil end\n"   /* nil/non-table -> C falls back to the built-in template */
"  return res\n"
"end\n";

int cel_hooks_install(cel_lua_t *L, char *errbuf, size_t errlen) {
    return cel_lua_dostring(L, PRELUDE, errbuf, errlen);
}

/* ---- dispatch -------------------------------------------------------------- */
int cel_hooks_authorize(cel_lua_t *Lh, const char *op, const char *table,
                        const cel_val_t *row, const cel_val_t *who) {
    lua_State *L = (lua_State *)cel_lua_state(Lh);
    if (!L) return 0;   /* no VM → fail closed */
    lua_getglobal(L, "__cel_authorize");
    lua_pushstring(L, op ? op : "");
    lua_pushstring(L, table ? table : "");
    lua_pushlightuserdata(L, (void *)row);
    lua_pushlightuserdata(L, (void *)who);
    if (lua_pcall(L, 4, 1, 0) != 0) {
        LOG_ERROR("[hook] authorize fault: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;       /* fault → deny */
    }
    int allow = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return allow ? 1 : 0;
}

int cel_hooks_before(cel_lua_t *Lh, const char *op, const char *table,
                     cel_val_t *input, const cel_val_t *who,
                     char *errbuf, size_t errlen) {
    if (errbuf && errlen) errbuf[0] = '\0';
    lua_State *L = (lua_State *)cel_lua_state(Lh);
    if (!L) { if (errbuf && errlen) snprintf(errbuf, errlen, "no hook VM"); return -1; }
    lua_getglobal(L, "__cel_before");
    lua_pushstring(L, op ? op : "");
    lua_pushstring(L, table ? table : "");
    lua_pushlightuserdata(L, (void *)input);
    lua_pushlightuserdata(L, (void *)who);
    if (lua_pcall(L, 4, 2, 0) != 0) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "%s", lua_tostring(L, -1));
        LOG_ERROR("[hook] before fault: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;      /* fault → reject */
    }
    /* returns (ok, reason): ok==false → reject with reason */
    int ok = lua_toboolean(L, -2);
    if (!ok) {
        const char *reason = lua_tostring(L, -1);
        if (errbuf && errlen) snprintf(errbuf, errlen, "%s", reason ? reason : "rejected");
    }
    lua_pop(L, 2);
    return ok ? 0 : -1;
}

/* Is the table at absolute index `idx` a contiguous 1..n sequence (→ JSON array)
 * rather than a map (→ JSON object)? */
static int table_is_seq(lua_State *L, int idx) {
    size_t len = lua_objlen(L, idx);
    if (len == 0) return 0;
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {              /* key at -2, value at -1 */
        if (lua_type(L, -2) != LUA_TNUMBER) { lua_pop(L, 2); return 0; }
        double k = lua_tonumber(L, -2);
        if (k < 1 || k > (double)len || k != (double)(long long)k) { lua_pop(L, 2); return 0; }
        count++;
        lua_pop(L, 1);                           /* keep key for next */
    }
    return count == (int)len;
}

/* Marshal the Lua value at `idx` into an owned cJSON tree (rpc result path —
 * the only place a hook PRODUCES structured data). Depth-bounded against cycles. */
static cJSON *lua_to_cval(lua_State *L, int idx, int depth) {
    if (idx < 0) idx = lua_gettop(L) + idx + 1;  /* absolute (stable across pushes) */
    if (depth > 32) return cJSON_CreateNull();
    switch (lua_type(L, idx)) {
        case LUA_TNIL:     return cJSON_CreateNull();
        case LUA_TBOOLEAN: return cJSON_CreateBool(lua_toboolean(L, idx));
        case LUA_TNUMBER:  return cJSON_CreateNumber(lua_tonumber(L, idx));
        case LUA_TSTRING:  return cJSON_CreateString(lua_tostring(L, idx));
        case LUA_TTABLE:
            if (table_is_seq(L, idx)) {
                cJSON *a = cJSON_CreateArray();
                size_t n = lua_objlen(L, idx);
                for (size_t i = 1; i <= n; i++) {
                    lua_rawgeti(L, idx, (int)i);
                    cJSON_AddItemToArray(a, lua_to_cval(L, -1, depth + 1));
                    lua_pop(L, 1);
                }
                return a;
            } else {
                cJSON *o = cJSON_CreateObject();
                lua_pushnil(L);
                while (lua_next(L, idx) != 0) {   /* key at -2, value at -1 */
                    char keybuf[64];
                    const char *key = NULL;
                    if (lua_type(L, -2) == LUA_TSTRING) {
                        key = lua_tostring(L, -2);            /* string key: safe to read */
                    } else if (lua_type(L, -2) == LUA_TNUMBER) {
                        snprintf(keybuf, sizeof keybuf, "%g", lua_tonumber(L, -2));  /* don't tostring a number key mid-traversal */
                        key = keybuf;
                    }
                    if (key) cJSON_AddItemToObject(o, key, lua_to_cval(L, -1, depth + 1));
                    lua_pop(L, 1);
                }
                return o;
            }
        default: return cJSON_CreateNull();      /* function / userdata / cdata */
    }
}

/* Run up to `budget` due jobs from `q`, dispatching each to the Lua `job` hook
 * (on this thread's state). Completes on success, fails-with-retry on a Lua error
 * or a job() error. The caller binds a db connection (cel_hooks_set_db) so the
 * job handler's cellar.query/exec run against it. Returns the number processed. */
int cel_hooks_run_jobs(cel_lua_t *Lh, job_queue_t *q, long long now,
                       int visibility, int budget) {
    lua_State *L = (lua_State *)cel_lua_state(Lh);
    if (!L || !q) return 0;
    int processed = 0;
    for (int i = 0; i < budget; i++) {
        job_t job;
        int rc = q->claim(q->ctx, now, visibility, &job);
        if (rc != JOBQ_OK) break;             /* JOBQ_NONE (drained) or error */
        /* Reserved engine job: NotifChannel fan-out, handled in C (not the Lua hook).
         * Off-site delivery does network I/O (SMTP / push HTTP) and only READS the db
         * to resolve recipients — drop the app write lock around it so a slow/timing-out
         * send can't stall request-thread writes, then re-take it for the rest of the
         * batch. A transient channel failure retries with backoff (like the Lua path). */
        if (job.type && strcmp(job.type, CEL_NOTIF_JOB_TYPE) == 0) {
            app_db_t *napp = app_db_current();
            if (napp) app_db_write_unlock(napp);
            int nrc = cel_notif_fanout(Lh, job.payload);
            if (napp) app_db_write_lock(napp);
            if (nrc < 0) q->fail(q->ctx, job.id, "notify delivery failed", jobq_backoff_at(now, job.attempt));
            else         q->complete(q->ctx, job.id, now);
            job_free(&job);
            processed++;
            continue;
        }
        lua_getglobal(L, "__cel_job");
        lua_pushstring(L, job.type ? job.type : "");
        lua_pushstring(L, job.payload ? job.payload : "");
        int ok;
        if (lua_pcall(L, 2, 2, 0) != 0) {
            LOG_ERROR("[hook] job fault: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
            ok = 0;
        } else {
            ok = lua_toboolean(L, -2);
            lua_pop(L, 2);
        }
        if (ok) q->complete(q->ctx, job.id, now);
        else    q->fail(q->ctx, job.id, "job handler failed", jobq_backoff_at(now, job.attempt));
        job_free(&job);
        processed++;
    }
    return processed;
}

cel_val_t *cel_hooks_rpc(cel_lua_t *Lh, const char *name, const cel_val_t *args,
                         const cel_val_t *who, char *errbuf, size_t errlen) {
    if (errbuf && errlen) errbuf[0] = '\0';
    lua_State *L = (lua_State *)cel_lua_state(Lh);
    if (!L) { if (errbuf && errlen) snprintf(errbuf, errlen, "no hook VM"); return NULL; }
    lua_getglobal(L, "__cel_rpc");
    lua_pushstring(L, name ? name : "");
    lua_pushlightuserdata(L, (void *)args);
    lua_pushlightuserdata(L, (void *)who);
    if (lua_pcall(L, 3, 2, 0) != 0) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "%s", lua_tostring(L, -1));
        LOG_ERROR("[hook] rpc fault: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return NULL;
    }
    /* trampoline returns (result, err): a nil result means the error path */
    if (lua_isnil(L, -2)) {
        const char *m = lua_tostring(L, -1);
        if (errbuf && errlen) snprintf(errbuf, errlen, "%s", m ? m : "rpc failed");
        lua_pop(L, 2);
        return NULL;
    }
    cJSON *result = lua_to_cval(L, -2, 0);
    lua_pop(L, 2);
    return (cel_val_t *)result;
}

cel_val_t *cel_hooks_render_email(cel_lua_t *Lh, const char *kind, const cel_val_t *ctx) {
    lua_State *L = (lua_State *)cel_lua_state(Lh);
    if (!L) return NULL;
    lua_getglobal(L, "__cel_render_email");
    lua_pushstring(L, kind ? kind : "");
    lua_pushlightuserdata(L, (void *)ctx);
    if (lua_pcall(L, 2, 1, 0) != 0) {                    /* hook faulted → fall back */
        LOG_ERROR("[hook] render_email fault: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return NULL;
    }
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return NULL; } /* no/opt-out hook → fall back */
    cJSON *result = lua_to_cval(L, -1, 0);
    lua_pop(L, 1);
    return (cel_val_t *)result;
}

void cel_hooks_after(cel_lua_t *Lh, const char *op, const char *table,
                     const cel_val_t *row, const cel_val_t *who) {
    lua_State *L = (lua_State *)cel_lua_state(Lh);
    if (!L) return;
    lua_getglobal(L, "__cel_after");
    lua_pushstring(L, op ? op : "");
    lua_pushstring(L, table ? table : "");
    lua_pushlightuserdata(L, (void *)row);
    lua_pushlightuserdata(L, (void *)who);
    if (lua_pcall(L, 4, 0, 0) != 0) {
        LOG_ERROR("[hook] after fault (ignored; write already committed): %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

int cel_hooks_on_realtime(cel_lua_t *Lh, const cel_val_t *change, const cel_val_t *subscriber) {
    lua_State *L = (lua_State *)cel_lua_state(Lh);
    if (!L) return 1;   /* no VM → deliver */
    lua_getglobal(L, "__cel_on_realtime");
    lua_pushlightuserdata(L, (void *)change);
    lua_pushlightuserdata(L, (void *)subscriber);
    if (lua_pcall(L, 2, 1, 0) != 0) {
        LOG_ERROR("[hook] on_realtime fault (dropping subscriber): %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;       /* fault → fail closed (don't deliver) */
    }
    int deliver = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return deliver ? 1 : 0;
}

int cel_hooks_resolve(cel_lua_t *Lh, const char *table, const cel_val_t *incoming,
                      const cel_val_t *current, const cel_val_t *who) {
    lua_State *L = (lua_State *)cel_lua_state(Lh);
    if (!L) return 1;   /* no VM → LWW (incoming wins) — the default policy */
    lua_getglobal(L, "__cel_resolve");
    lua_pushstring(L, table ? table : "");
    if (incoming) lua_pushlightuserdata(L, (void *)incoming);
    else          lua_pushnil(L);                 /* del has no incoming row */
    lua_pushlightuserdata(L, (void *)current);
    lua_pushlightuserdata(L, (void *)who);
    if (lua_pcall(L, 4, 1, 0) != 0) {
        LOG_ERROR("[hook] resolve fault (keeping current): %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;       /* fault → fail closed: keep current, don't apply a faulted merge */
    }
    const char *r = lua_tostring(L, -1);
    int incoming_wins = (r && strcmp(r, "incoming") == 0) ? 1 : 0;
    lua_pop(L, 1);
    return incoming_wins;
}
