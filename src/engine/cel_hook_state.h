/* ============================================================================
 * cellar — per-app hook VM cache (design §9).
 *
 * One LuaJIT state per (worker thread × app): thread-local, lazily created from
 * the bundle's hooks.lua, reused across requests, and hot-reloaded when hooks.lua
 * changes on disk. Per the §9 model this needs NO mutex on the hot path — states
 * aren't shared between threads, so authorize/on_realtime on the WAL-concurrent
 * read path never serialize. A small per-app lock guards only the shared reload
 * bookkeeping (the mtime + generation counter), held for a stat, never for Lua.
 * ============================================================================ */
#ifndef CEL_HOOK_STATE_H
#define CEL_HOOK_STATE_H

#include "cel_lua.h"

/* Per-app hook metadata, shared across threads (one per app). */
typedef struct cel_hook_app cel_hook_app_t;

/* Create an app's hook metadata from its bundle directory (looks for
 * <bundle_dir>/hooks.lua). A bundle with no hooks.lua is valid — every state is
 * then NULL and hooks no-op. Returns NULL only on allocation failure. */
cel_hook_app_t *cel_hook_app_create(const char *bundle_dir);

/* Destroy an app's hook metadata. Does NOT free per-thread states (worker threads
 * own those for their lifetime); call from app teardown after workers are joined. */
void cel_hook_app_destroy(cel_hook_app_t *app);

/* This thread's lua_State for the app, created on first use (prelude + hooks.lua)
 * and transparently reloaded if hooks.lua changed since this thread last loaded.
 * Returns NULL when there is no hooks.lua, or it failed to compile (logged, and
 * cached until the next file change so we don't retry every request). Callers
 * treat NULL as "no hooks" — authorize allows, before accepts, rpc is absent. */
cel_lua_t *cel_hook_app_state(cel_hook_app_t *app);

/* Free THIS thread's cached states (closes each lua_State). A worker thread calls
 * it on exit for a clean teardown; not required for correctness (process exit
 * reclaims everything), but it keeps shutdown leak-free. */
void cel_hook_state_thread_cleanup(void);

#endif /* CEL_HOOK_STATE_H */
