/* ============================================================================
 * cellar — the Lua hook dispatcher (design §8).
 *
 * Bridges the engine to a bundle's hooks.lua: installs the FFI cdef + the
 * `cellar` prelude (table-like sugar over the cel_val handle API) into a state,
 * then invokes the named hook globals around the request path. Payloads cross as
 * opaque cel_val_t* handles (boxed into Lua proxies by the prelude), never as
 * cJSON — option A from the §8 sketch.
 *
 * Every dispatch is pcall-isolated and FAILS CLOSED: a faulting authorize denies,
 * a faulting before rejects. An absent hook is a no-op (allow / accept).
 * ============================================================================ */
#ifndef CEL_HOOKS_H
#define CEL_HOOKS_H

#include <stddef.h>
#include "cel_lua.h"
#include "cel_val.h"

/* Install the FFI cdef + `cellar` prelude into a freshly opened state (before the
 * bundle's hooks.lua is loaded into it). Returns 0 on success, non-zero + errbuf
 * on a prelude error (should never happen — the prelude is engine-controlled). */
int cel_hooks_install(cel_lua_t *L, char *errbuf, size_t errlen);

/* Side-effect C API reachable from hooks via ffi.C (the binary must be linked
 * -rdynamic so the symbol resolves). level: 0=debug 1=info 2=warn 3=error. */
void cel_hook_log(int level, const char *msg);

/* authorize(op, table, row, who): an ADDITIONAL allow gate beyond the built-in
 * policy. Returns 1=allow, 0=deny. Absent hook → allow; a fault → deny. */
int cel_hooks_authorize(cel_lua_t *L, const char *op, const char *table,
                        const cel_val_t *row, const cel_val_t *who);

/* before(op, table, input, who): validate / default / transform. `input` is a
 * WRITABLE handle the hook may mutate in place. Returns 0=accept, -1=reject with
 * the (truncated) reason copied into errbuf. Absent hook → accept; a fault →
 * reject. */
int cel_hooks_before(cel_lua_t *L, const char *op, const char *table,
                     cel_val_t *input, const cel_val_t *who,
                     char *errbuf, size_t errlen);

#endif /* CEL_HOOKS_H */
