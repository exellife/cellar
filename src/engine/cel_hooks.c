/* cellar — the Lua hook dispatcher (design §8). See cel_hooks.h. */
#include "cel_hooks.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>

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
"  local out = deep(res)\n"
"  C.cel_val_free(res)\n"
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
"function __cel_rpc(name, args_ptr, who_ptr)\n"
"  if type(rpc) ~= 'function' then return nil, 'no rpc handler for ' .. tostring(name) end\n"
"  local res, errm = rpc(name, box(args_ptr, false), box(who_ptr, false))\n"
"  if res == nil then return nil, errm and tostring(errm) or 'rpc returned nil' end\n"
"  return res\n"   /* a Lua value; the C side marshals it back to JSON */
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
