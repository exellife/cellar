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

/* Bind (or clear, with NULL) the SQLite connection that this thread's hook db
 * side-effects (cellar.query/exec) run against — the caller sets it around a hook
 * dispatch to the request's connection. Opaque (really sqlite3 *) to keep sqlite
 * out of this header. */
void cel_hooks_set_db(void *sqlite3_conn);

/* ---- side-effect C API reachable from hooks via ffi.C ----
 * (the binary must be linked -rdynamic so these symbols resolve). */
void       cel_hook_log  (int level, const char *msg);       /* 0=dbg 1=info 2=warn 3=err */
/* parameterized query against this thread's bound db → an owned array-of-row-
 * objects value (caller frees with cel_val_free), or NULL + err. */
cel_val_t *cel_hook_query(const char *sql, const cel_val_t *params, char *err, int errlen);
/* parameterized statement → rows changed, or -1 + err. */
long long  cel_hook_exec (const char *sql, const cel_val_t *params, char *err, int errlen);
/* EventSink emit onto the current app's log (best-effort; no error path). */
void       cel_hook_emit (const char *type, const char *actor, const char *subject, const char *props);
/* JobQueue enqueue onto the current app's queue → new id, or -1. */
long long  cel_hook_enqueue(const char *type, const char *payload, long long run_at, long long repeat_every);
/* Realtime publish for a server-created row (e.g. a notification) → live subscribers. */
void       cel_hook_rt_emit(const char *table, const char *op, const char *row_json);
/* NotifChannel off-site fan-out (cellar.notify): enqueue a cel:notif job for `user_id`
 * with the message; cel_hooks_run_jobs delivers it via the registered channels. id/-1. */
long long  cel_hook_notify(const char *user_id, const char *title, const char *body,
                           const char *url, const char *data_json);
/* Create a login account (cellar.create_user) → 0 + new id in out_id, or -1 + err.
 * The actual creator is wired by the engine at boot to keep this layer free of the
 * auth/crypto deps; unwired (e.g. in unit tests) it returns -1 ("not available").
 * The bundle's rpc enforces who-may-create-whom; this refuses platform_admin. */
int        cel_hook_create_user(const char *email, const char *password, const char *role,
                                char *out_id, int out_id_size, char *err, int errlen);
/* Wire the account creator (cel_auth_create_user, via an adapter in main). */
typedef int (*cel_hook_create_user_fn)(const char *email, const char *password, const char *role,
                                       char *out_id, int out_id_size, char *err, int errlen);
void       cel_hooks_set_user_creator(cel_hook_create_user_fn fn);

/* Set (reset) an existing account's password (cellar.set_password) → 0, or -1 + err.
 * The setter is wired by the engine at boot (same auth/crypto decoupling as the
 * creator); unwired it returns -1. The bundle's rpc enforces who-may-reset-whom;
 * the wired adapter refuses resetting a superuser. */
int        cel_hook_set_password(const char *email, const char *new_password, char *err, int errlen);
/* Wire the password setter (an adapter in main: superuser floor + cel_auth_set_password). */
typedef int (*cel_hook_set_password_fn)(const char *email, const char *new_password, char *err, int errlen);
void       cel_hooks_set_password_setter(cel_hook_set_password_fn fn);

/* Claim + dispatch up to `budget` due jobs from `q` (job_queue_t*) to the Lua
 * `job` hook on this thread's state; completes/retries per result. The caller
 * binds a db connection (cel_hooks_set_db) for the handlers. Returns # processed. */
struct job_queue;
int cel_hooks_run_jobs(cel_lua_t *L, struct job_queue *q, long long now,
                       int visibility, int budget);

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

/* after(op, table, row, who): post-commit side effects (notify, audit, enqueue).
 * Return value ignored; a fault is logged but never fails the request (the write
 * already committed). Absent hook → no-op. */
void cel_hooks_after(cel_lua_t *L, const char *op, const char *table,
                     const cel_val_t *row, const cel_val_t *who);

/* rpc(name, args, who): a custom endpoint beyond CRUD. Returns an OWNED result
 * value (caller frees with cel_val_free) on success, or NULL with the reason in
 * errbuf — absent handler, a hook that returned nil, or a fault. */
cel_val_t *cel_hooks_rpc(cel_lua_t *L, const char *name, const cel_val_t *args,
                         const cel_val_t *who, char *errbuf, size_t errlen);

/* render_email(kind, ctx): let the bundle produce a branded email body. `kind` is
 * "verify_email" | "password_reset" | "notification"; `ctx` is a value object with
 * the relevant fields (url, token, email, title, body, …). Returns an OWNED result
 * (caller frees with cel_val_free) — a table {subject?, html?, text?} — or NULL when
 * the app defines no render_email, returns a non-table, or faults. In every NULL
 * case the caller falls back to the engine's built-in plain template. */
cel_val_t *cel_hooks_render_email(cel_lua_t *L, const char *kind, const cel_val_t *ctx);

/* on_realtime(change, subscriber): a delivery filter for realtime change events.
 * Returns 1 to deliver this change to this subscriber, 0 to drop it. Absent hook →
 * deliver; a fault → drop (fail closed). A pure filter — no db is bound. */
int cel_hooks_on_realtime(cel_lua_t *L, const cel_val_t *change, const cel_val_t *subscriber);

/* resolve(table, incoming, current, who): conflict resolution for sync_push. Returns
 * 1 if the incoming write wins (apply it), 0 if the current row wins (keep it).
 * Absent hook / no VM → 1 (last-write-wins, the default). A fault → 0 (fail closed:
 * keep current rather than apply a write whose merge logic errored). `incoming` is
 * the client's row (nil for a delete); `current` is the server's stored row. */
int cel_hooks_resolve(cel_lua_t *L, const char *table, const cel_val_t *incoming,
                      const cel_val_t *current, const cel_val_t *who);

#endif /* CEL_HOOKS_H */
