/* ============================================================================
 * cellar — the control-plane registry (design §11).
 *
 * A small SQLite database, SEPARATE from any app's data.db, that holds what isn't
 * per-app: the app registry (host → status) and its lifecycle. When a control DB
 * is configured (CEL_CONTROL_DB), routing becomes an explicit allow-list — only a
 * host that is REGISTERED and ACTIVE is served; suspending an app takes it offline
 * (the server re-reads status per request) without deleting its bundle. With no
 * control DB configured, routing keeps the simpler "any bundle dir serves"
 * behavior. (Platform admins / global ops are a later addition.)
 * ============================================================================ */
#ifndef CEL_CONTROL_H
#define CEL_CONTROL_H

#include <stdbool.h>

/* Open (creating + migrating) the control DB at `path`. Idempotent. 0 on success. */
int  cel_control_open(const char *path);
void cel_control_close(void);

/* Register a host (no-op if already registered), defaulting to status 'active'. */
int  cel_control_register(const char *host);

/* Set a registered host's status ("active" | "suspended"). Returns 1 if a row was
 * updated, 0 if the host isn't registered, -1 on error. */
int  cel_control_set_status(const char *host, const char *status);

/* Remove a host from the registry (does NOT touch its bundle files). Returns rows
 * removed (0 or 1), or -1 on error. */
int  cel_control_unregister(const char *host);

/* True iff `host` is registered AND its status is 'active' — the routing gate. */
bool cel_control_is_active(const char *host);

/* Iterate the registry (host, status, created_at epoch) in host order. */
void cel_control_list(void (*cb)(const char *host, const char *status,
                                 long long created_at, void *ud), void *ud);

#endif /* CEL_CONTROL_H */
