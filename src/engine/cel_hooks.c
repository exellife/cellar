/* cellar — the Lua hook dispatcher (design §8). See cel_hooks.h. */
#include "cel_hooks.h"
#include "logger.h"

#include <string.h>

#include "lua.h"
#include "lauxlib.h"

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
"  void cel_hook_log(int, const char*);\n"
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
"-- trampolines (called from C; user hooks are the globals authorize/before)\n"
"function __cel_authorize(op, tbl, row_ptr, who_ptr)\n"
"  if type(authorize) ~= 'function' then return true end\n"
"  return authorize(op, tbl, box(row_ptr, false), box(who_ptr, false)) and true or false\n"
"end\n"
"function __cel_before(op, tbl, input_ptr, who_ptr)\n"
"  if type(before) ~= 'function' then return true end\n"
"  local ok, reason = before(op, tbl, box(input_ptr, true), box(who_ptr, false))\n"
"  if ok == false then return false, reason and tostring(reason) or 'rejected' end\n"
"  return true\n"
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
