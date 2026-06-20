/* ============================================================================
 * cellar — LuaJIT hook VM (design §7-9).
 *
 * Phase-2 foundation: a thin wrapper over a LuaJIT `lua_State` with the standard
 * libraries + FFI open, and pcall-isolated execution so a broken hook surfaces
 * as an error string, never a longjmp past us or a process crash.
 *
 * The hook contract (authorize/before/after/rpc/on_realtime) and the per-app,
 * per-thread state cache (one state per worker-thread × app, hot-reloaded on
 * hooks.lua change) layer on top of this — kept out of here so the lifecycle is
 * unit-testable on its own.
 * ============================================================================ */
#ifndef CEL_LUA_H
#define CEL_LUA_H

#include <stddef.h>

typedef struct cel_lua cel_lua_t;

/* Create a Lua state with the standard libraries + FFI/JIT open. NULL on OOM. */
cel_lua_t *cel_lua_open(void);

/* Destroy a state (no-op on NULL). */
void cel_lua_close(cel_lua_t *L);

/* Compile + run a chunk of Lua source under pcall. Returns 0 on success. On a
 * compile or runtime error returns non-zero and copies the (truncated) Lua error
 * message into errbuf (when given) — the error is contained, not propagated. */
int cel_lua_dostring(cel_lua_t *L, const char *src, char *errbuf, size_t errlen);

/* The underlying LuaJIT state, as an opaque pointer (really lua_State *). For the
 * hook dispatcher (cel_hooks) which needs the raw stack API; kept void* so this
 * header doesn't drag in lua.h. NULL if the state is gone. */
void *cel_lua_state(cel_lua_t *L);

#endif /* CEL_LUA_H */
