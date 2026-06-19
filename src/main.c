/* ============================================================================
 * pgforge — server entry point (Phase 0: boot + ping/echo)
 *
 * Boots the platform that the schema-driven engine will plug into:
 *   logger -> opcode dispatcher (thread pools) -> WebSocket listener (wslib).
 *
 * Incoming binary frames are parsed into the pgf_message_t header, wrapped in an
 * opcode_context, and dispatched by opcode. Generic data-layer handlers (DB_*)
 * land here in later phases; for now only the system ops are registered.
 * ============================================================================ */
#include "wslib.h"
#include "opcode_dispatcher.h"
#include "logger.h"
#include "core/protocol.h"
#include "core/db_connection.h"
#include "core/password.h"
#include "core/auth.h"
#include "core/session_cache.h"
#include "core/rate_limit.h"
#include "core/metrics.h"
#include "core/mfa.h"
#include "core/oauth.h"
#include "core/mailer.h"
#include "core/cors.h"
#include "core/migrate.h"

#include <curl/curl.h>
#include "engine/schema_catalog.h"
#include "engine/policy.h"
#include "engine/api.h"   /* pgf_rpc_audit_security_definer, pgf_api_rt_recheck_member */
#include "handlers/auth_handlers.h"
#include "handlers/schema_handlers.h"
#include "handlers/data_handlers.h"
#include "handlers/realtime_handlers.h"
#include "handlers/http_routes.h"
#include "engine/realtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define PGF_VERSION "0.1.0"
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <arpa/inet.h>   /* htons/htonl/ntohs/ntohl */

/* ---- global state ---------------------------------------------------------- */
static ws_server_t         *g_server     = NULL;
static opcode_dispatcher_t *g_dispatcher = NULL;
static volatile sig_atomic_t g_running   = 1;
static pgf_ratelimit_t     *g_auth_rl    = NULL;   /* auth-endpoint throttle */
static pgf_ratelimit_t     *g_api_rl     = NULL;   /* data-API throttle */

/* ---- response helpers ------------------------------------------------------ */

/* Frame `payload` with the 8-byte header (response bit set) and send it. */
static void pgf_send(int fd, uint8_t opcode, uint8_t flags, uint16_t message_id,
                     const void *payload, size_t payload_size) {
    size_t total = PGF_HEADER_SIZE + payload_size;
    uint8_t *buf = malloc(total);
    if (!buf) return;

    pgf_message_t *msg = (pgf_message_t *)buf;
    msg->opcode         = opcode | PGF_FLAG_RESPONSE;
    msg->flags          = flags;
    msg->message_id     = htons(message_id);
    msg->payload_length = htonl((uint32_t)payload_size);
    if (payload_size) memcpy(msg->payload, payload, payload_size);

    ws_send_binary(g_server, fd, buf, total);
    free(buf);
}

static void pgf_send_error(int fd, uint8_t opcode, uint16_t message_id,
                           const char *why) {
    pgf_send(fd, opcode, PGF_FLAG_ERROR, message_id, why, strlen(why));
}

/* Realtime push: frame a CHANGE as a server-initiated message (no RESPONSE flag,
 * message_id 0) and send it. Registered with the realtime module at startup. */
static void rt_push(int fd, const char *json, size_t len) {
    size_t total = PGF_HEADER_SIZE + len;
    uint8_t *buf = malloc(total);
    if (!buf) return;
    pgf_message_t *msg = (pgf_message_t *)buf;
    msg->opcode         = OP_CHANGE;
    msg->flags          = 0;
    msg->message_id     = htons(0);
    msg->payload_length = htonl((uint32_t)len);
    if (len) memcpy(msg->payload, json, len);
    ws_send_binary(g_server, fd, buf, total);
    free(buf);
}

/* ---- opcode handlers ------------------------------------------------------- */
/* INLINE handlers run synchronously on the caller thread and set ctx->response_* */

static handler_result_t set_response(opcode_context_t *ctx,
                                     const void *data, size_t size) {
    void *copy = malloc(size ? size : 1);
    if (!copy) return RESULT_ERROR;
    if (size) memcpy(copy, data, size);
    ctx->response_data = copy;
    ctx->response_size = size;
    ctx->owns_data     = true;
    return RESULT_SUCCESS;
}

static handler_result_t handle_ping(opcode_context_t *ctx) {
    return set_response(ctx, "PONG", 4);
}

static handler_result_t handle_echo(opcode_context_t *ctx) {
    return set_response(ctx, ctx->data, ctx->data_size);
}

static handler_result_t handle_server_info(opcode_context_t *ctx) {
    static const char info[] = "{\"name\":\"pgforge\",\"version\":\"" PGF_VERSION "\"}";
    return set_response(ctx, info, sizeof(info) - 1);
}

static const opcode_handler_descriptor_t HANDLERS[] = {
    /* system (synchronous) */
    {OP_PING,           handle_ping,                POOL_INLINE, "ping"},
    {OP_ECHO,           handle_echo,                POOL_INLINE, "echo"},
    {OP_SERVER_INFO,    handle_server_info,         POOL_INLINE, "server_info"},
    /* auth (DB-backed, run on the DB pool) */
    {OP_LOGIN,          pgf_handle_login,           POOL_DB,     "login"},
    {OP_VERIFY_SESSION, pgf_handle_verify_session,  POOL_DB,     "verify_session"},
    {OP_LOGOUT,         pgf_handle_logout,          POOL_DB,     "logout"},
    /* realtime: subscribe verifies token + access rules (DB); unsubscribe is local */
    {OP_SUBSCRIBE,      pgf_handle_subscribe,       POOL_DB,     "subscribe"},
    {OP_UNSUBSCRIBE,    pgf_handle_unsubscribe,     POOL_INLINE, "unsubscribe"},
    /* schema-driven data layer (schema now verifies a token -> DB pool) */
    {OP_DB_SCHEMA,      pgf_handle_db_schema,       POOL_DB,     "db_schema"},
    {OP_DB_LIST,        pgf_handle_db_list,         POOL_DB,     "db_list"},
    {OP_DB_GET,         pgf_handle_db_get,          POOL_DB,     "db_get"},
    {OP_DB_CREATE,      pgf_handle_db_create,       POOL_DB,     "db_create"},
    {OP_DB_UPDATE,      pgf_handle_db_update,       POOL_DB,     "db_update"},
    {OP_DB_DELETE,      pgf_handle_db_delete,       POOL_DB,     "db_delete"},
    {OP_DB_RPC,         pgf_handle_db_rpc,          POOL_DB,     "db_rpc"},
    {OP_DB_QUERY,       pgf_handle_db_query,        POOL_DB,     "db_query"},
};
#define HANDLER_COUNT (sizeof(HANDLERS) / sizeof(HANDLERS[0]))

/* ---- async completion (for POOL_CPU / POOL_DB handlers, used later) -------- */
static void async_completion(opcode_context_t *ctx, const void *data,
                             size_t size, void *user_data) {
    (void)data; (void)user_data;
    int fd = (int)(intptr_t)ctx->user_data;
    if (ctx->response_data && ctx->response_size > 0) {
        uint8_t flags = (ctx->flags & PGF_FLAG_ERROR);
        pgf_send(fd, (uint8_t)ctx->opcode, flags, (uint16_t)ctx->message_id,
                 ctx->response_data, ctx->response_size);
    } else {
        pgf_send_error(fd, (uint8_t)ctx->opcode, (uint16_t)ctx->message_id,
                       "handler produced no response");
    }
    opcode_context_destroy(ctx);
}

/* ---- wslib callbacks ------------------------------------------------------- */
static int on_connect(int fd, void *user_data) {
    (void)user_data;
    LOG_DEBUG("connection established: fd=%d", fd);
    return 0;
}

static void on_disconnect(int fd, void *user_data) {
    (void)user_data;
    pgf_realtime_drop_conn(fd);   /* tear down this connection's subscriptions */
    LOG_DEBUG("connection closed: fd=%d", fd);
}

/* Sampled at /metrics scrape time: pull live gauges from the subsystems that own
 * them (keeps core/metrics a dependency-free leaf). Active connections come from
 * portico (mechanism); the rest from our own subsystems. */
static void metrics_gauges(pgf_gauges_t *g) {
    db_connection_pool_stats(&g->db_pool_size, &g->db_pool_in_use);
    g->realtime_subscriptions = pgf_realtime_count();
    g->active_connections     = portico_active_connections(g_server);
}

static int on_binary_message(int fd, const void *data, size_t len, void *user_data) {
    (void)user_data;

    if (len < PGF_HEADER_SIZE) {
        LOG_WARN("short frame (%zu bytes) from fd=%d", len, fd);
        return -1;
    }
    const pgf_message_t *msg = (const pgf_message_t *)data;
    uint32_t payload_len = ntohl(msg->payload_length);
    if (len != PGF_HEADER_SIZE + payload_len) {
        LOG_WARN("length mismatch (got %zu, expected %u) from fd=%d",
                 len, PGF_HEADER_SIZE + payload_len, fd);
        return -1;
    }
    uint16_t message_id = ntohs(msg->message_id);

    opcode_context_t *ctx =
        opcode_context_create(msg->opcode, message_id, msg->payload, payload_len);
    if (!ctx) {
        LOG_ERROR("failed to create context for fd=%d", fd);
        return -1;
    }
    ctx->user_data = (void *)(intptr_t)fd;
    opcode_context_set_callback(ctx, async_completion, NULL);

    handler_result_t r = opcode_dispatcher_dispatch(g_dispatcher, ctx);
    if (r == RESULT_SUCCESS && ctx->response_data) {
        pgf_send(fd, msg->opcode, 0, message_id,
                 ctx->response_data, ctx->response_size);
        opcode_context_destroy(ctx);
    } else if (r == RESULT_ASYNC) {
        /* thread pool owns ctx; async_completion will fire */
    } else {
        LOG_ERROR("dispatch error for opcode 0x%02X from fd=%d", msg->opcode, fd);
        pgf_send_error(fd, msg->opcode, message_id, "no handler / error");
        opcode_context_destroy(ctx);
    }
    return 0;
}

/* ---- lifecycle ------------------------------------------------------------- */
static void on_signal(int sig) { (void)sig; g_running = 0; }

static int env_int(const char *name, int fallback) {
    const char *v = getenv(name);
    return (v && *v) ? atoi(v) : fallback;
}

static const char *env_str(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return (v && *v) ? v : fallback;
}

/* Bring up the libpq connection pool from PGF_DB_* env vars.
 * Returns 0 on success; non-zero leaves login disabled but the server runs. */
static int init_db(void) {
    /* Only emit password=... when explicitly provided; otherwise let libpq fall
     * back to ~/.pgpass / PGPASSWORD (an empty password= field disables that). */
    const char *pw = env_str("PGF_DB_PASSWORD", "");
    char pwfield[256] = "";
    if (*pw) snprintf(pwfield, sizeof pwfield, "password=%s ", pw);

    /* A PGF_DB_HOST starting with '/' is a Unix-domain socket directory (e.g.
     * /var/run/postgresql) — cheaper than TCP for a co-located Postgres. TCP
     * keepalive params are meaningless on a socket, so omit them there. */
    const char *host = env_str("PGF_DB_HOST", "localhost");
    int is_socket = (host[0] == '/');
    const char *keepalives = is_socket ? ""
        : "keepalives=1 keepalives_idle=30 keepalives_interval=10 keepalives_count=5";

    char conninfo[512];
    snprintf(conninfo, sizeof conninfo,
             "host=%s port=%s dbname=%s user=%s %s%s",
             host,
             env_str("PGF_DB_PORT", "5432"),
             env_str("PGF_DB_NAME", "pgforge"),
             env_str("PGF_DB_USER", "postgres"),
             pwfield, keepalives);
    LOG_INFO("Database transport: %s (host=%s)", is_socket ? "unix-socket" : "tcp", host);
    return db_connection_pool_init(conninfo, env_int("PGF_DB_POOL", 8));
}

/* Optional first-run seeding: PGF_SEED_ADMIN="email:password". */
static void maybe_seed_admin(void) {
    const char *spec = getenv("PGF_SEED_ADMIN");
    if (!spec || !*spec) return;
    const char *colon = strchr(spec, ':');
    if (!colon || colon == spec || !colon[1]) {
        LOG_WARN("PGF_SEED_ADMIN must be 'email:password' — skipping");
        return;
    }
    char email[256];
    size_t elen = (size_t)(colon - spec);
    if (elen >= sizeof email) { LOG_WARN("PGF_SEED_ADMIN email too long"); return; }
    memcpy(email, spec, elen);
    email[elen] = '\0';
    if (pgf_auth_seed_admin(email, colon + 1) == 0)
        LOG_INFO("seeded admin user '%s'", email);
    else
        LOG_ERROR("failed to seed admin user '%s'", email);
}

/* Optional multi-user seeding: PGF_SEED_USERS="email:pass:role;email:pass:role". */
static void maybe_seed_users(void) {
    const char *spec = getenv("PGF_SEED_USERS");
    if (!spec || !*spec) return;
    char *dup = strdup(spec);
    if (!dup) return;
    char *save = NULL;
    for (char *rec = strtok_r(dup, ";", &save); rec; rec = strtok_r(NULL, ";", &save)) {
        char *c1 = strchr(rec, ':');
        char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        if (!c1 || !c2) { LOG_WARN("PGF_SEED_USERS entry must be email:pass:role"); continue; }
        *c1 = '\0'; *c2 = '\0';
        const char *em = rec, *pw = c1 + 1, *role = c2 + 1;
        if (pgf_auth_seed_user(em, pw, role) == 0) LOG_INFO("seeded user '%s' (%s)", em, role);
        else                                       LOG_ERROR("failed to seed user '%s'", em);
    }
    free(dup);
}

/* `pgforge migrate [status] [--demo]` — apply embedded migrations and exit.
 * --demo also applies the sample-data migrations (sql/demo). */
static int run_migrate_command(int argc, char **argv) {
    int status = 0, with_demo = 0, with_tenancy = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "status")) status = 1;
        else if (!strcmp(argv[i], "--demo") || !strcmp(argv[i], "demo")) with_demo = 1;
        else if (!strcmp(argv[i], "--tenancy") || !strcmp(argv[i], "tenancy")) with_tenancy = 1;
    }
    const char *lvl = getenv("PGF_LOG_LEVEL");
    logger_init(lvl ? logger_string_to_level(lvl) : LOG_LEVEL_INFO, NULL, 1);
    if (init_db() != 0) { LOG_ERROR("migrate: database unavailable"); logger_shutdown(); return 1; }

    int rc;
    if (status) {
        rc = pgf_migrate_status(with_demo, with_tenancy);
    } else {
        int applied = 0;
        rc = pgf_migrate_run(with_demo, with_tenancy, &applied);
        if (rc == 0) LOG_INFO("migrate: up to date (%d applied this run)", applied);
    }
    db_connection_pool_cleanup();
    logger_shutdown();
    return rc == 0 ? 0 : 1;
}

/* `pgforge tenancy-protect` — enable Postgres RLS + the tenant-isolation policy on
 * every tenant-scoped table (pooled-mode defense-in-depth). Re-run after adding
 * new tenant tables. Requires PGF_TENANT_COLUMN (pooled mode). */
static int run_tenancy_protect_command(void) {
    const char *lvl = getenv("PGF_LOG_LEVEL");
    logger_init(lvl ? logger_string_to_level(lvl) : LOG_LEVEL_INFO, NULL, 1);
    const char *col = getenv("PGF_TENANT_COLUMN");
    if (!col || !*col) {
        LOG_ERROR("tenancy-protect: pooled mode not enabled (set PGF_TENANT_COLUMN)");
        logger_shutdown();
        return 1;
    }
    if (init_db() != 0) { LOG_ERROR("tenancy-protect: database unavailable"); logger_shutdown(); return 1; }
    int rc = pgf_tenancy_protect(col);
    db_connection_pool_cleanup();
    logger_shutdown();
    return rc == 0 ? 0 : 1;
}

/* Common setup for the pooled-mode admin CLIs: init logger, require pooled mode,
 * open the DB pool. Returns 0 when ready, else a process exit code. */
static int tenancy_cli_setup(void) {
    const char *lvl = getenv("PGF_LOG_LEVEL");
    logger_init(lvl ? logger_string_to_level(lvl) : LOG_LEVEL_INFO, NULL, 1);
    const char *col = getenv("PGF_TENANT_COLUMN");
    if (!col || !*col) {
        LOG_ERROR("this command requires pooled mode (set PGF_TENANT_COLUMN)");
        logger_shutdown();
        return 1;
    }
    if (init_db() != 0) { LOG_ERROR("database unavailable"); logger_shutdown(); return 1; }
    return 0;
}
static int tenancy_cli_done(int ok) {
    db_connection_pool_cleanup();
    logger_shutdown();
    return ok ? 0 : 1;
}

/* `pgforge create-platform-admin <email> <password>` — the GLOBAL admin, created
 * out-of-band only (never via signup): role=platform_admin, tenant_id NULL. This
 * is the boundary that stops a tenant from escalating to platform. */
static int run_create_platform_admin(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: pgforge create-platform-admin <email> <password>\n"); return 2; }
    int s = tenancy_cli_setup(); if (s) return s;
    int ok = (pgf_auth_seed_user(argv[2], argv[3], "platform_admin") == 0);
    if (ok) LOG_INFO("platform admin '%s' created", argv[2]);
    return tenancy_cli_done(ok);
}

/* `pgforge create-tenant <name> <admin-email> <admin-password>` — provision a
 * tenant and its first tenant-admin (role=admin, scoped to the new tenant). */
static int run_create_tenant(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: pgforge create-tenant <name> <admin-email> <admin-password>\n"); return 2; }
    int s = tenancy_cli_setup(); if (s) return s;
    char tid[37] = {0};
    int ok = (pgf_tenant_create(argv[2], tid, sizeof tid) == 0)
          && (pgf_auth_seed_user(argv[3], argv[4], "admin") == 0)
          && (pgf_tenant_assign_user(argv[3], tid) == 0);
    if (ok) LOG_INFO("tenant '%s' (%s) created with admin '%s'", argv[2], tid, argv[3]);
    return tenancy_cli_done(ok);
}

/* `pgforge {suspend,resume}-tenant <name-or-id>` — toggle a tenant's access. */
static int run_set_tenant_active(int argc, char **argv, int active) {
    if (argc < 3) { fprintf(stderr, "usage: pgforge %s <tenant-name-or-id>\n", argv[1]); return 2; }
    int s = tenancy_cli_setup(); if (s) return s;
    int n = 0;
    int ok = (pgf_tenant_set_active(argv[2], active, &n) == 0);
    if (ok) {
        LOG_INFO("tenant '%s' %s (%d users affected)", argv[2], active ? "resumed" : "suspended", n);
        int sct = env_int("PGF_SESSION_CACHE_TTL", 0);   /* M-2: warn about cache latency */
        if (sct > 0)
            LOG_WARN("a running server with the session cache enabled may still honor the "
                     "previous tenant state for up to %ds (PGF_SESSION_CACHE_TTL)", sct);
    }
    return tenancy_cli_done(ok);
}

/* `pgforge export-tenant <name-or-id>` — write a tenant's data as a loadable SQL
 * script to STDOUT (the graduate-to-standalone escape hatch). Runs at ERROR log
 * level so only the script lands on stdout (info/warn would otherwise too). */
static int run_export_tenant(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: pgforge export-tenant <tenant-name-or-id>\n"); return 2; }
    logger_init(LOG_LEVEL_ERROR, NULL, 0);
    const char *col = getenv("PGF_TENANT_COLUMN");
    if (!col || !*col) {
        fprintf(stderr, "export-tenant: requires pooled mode (set PGF_TENANT_COLUMN)\n");
        logger_shutdown(); return 1;
    }
    if (init_db() != 0) { fprintf(stderr, "export-tenant: database unavailable\n"); logger_shutdown(); return 1; }
    int rc = pgf_tenant_export(argv[2], col);
    db_connection_pool_cleanup();
    logger_shutdown();
    return rc == 0 ? 0 : 1;
}

/* `pgforge revoke-sessions <email>` — force-logout: delete every session for a
 * user (e.g. after a credential compromise). Works in single-tenant or pooled
 * mode. NOTE: clears the serving instance's cache only if run in-process; a CLI
 * run deletes the DB rows, and any running server re-validates within its cache
 * TTL (0 = immediate). */
static int run_revoke_sessions(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: pgforge revoke-sessions <email>\n"); return 2; }
    const char *lvl = getenv("PGF_LOG_LEVEL");
    logger_init(lvl ? logger_string_to_level(lvl) : LOG_LEVEL_INFO, NULL, 1);
    if (init_db() != 0) { LOG_ERROR("database unavailable"); logger_shutdown(); return 1; }
    int n = pgf_auth_revoke_user_sessions(argv[2]);
    if (n >= 0) {
        LOG_INFO("revoked %d session(s) for '%s'", n, argv[2]);
        int sct = env_int("PGF_SESSION_CACHE_TTL", 0);   /* M-1: warn about cache latency */
        if (sct > 0)
            LOG_WARN("a running server with the session cache enabled may still honor these "
                     "tokens for up to %ds (PGF_SESSION_CACHE_TTL)", sct);
    } else        LOG_ERROR("revoke failed for '%s'", argv[2]);
    db_connection_pool_cleanup();
    logger_shutdown();
    return n >= 0 ? 0 : 1;
}

/* `pgforge mfa-reset <email>` — admin lockout recovery: remove a user's TOTP
 * enrollment so they can log in with just their password (and re-enroll). */
static int run_mfa_reset(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: pgforge mfa-reset <email>\n"); return 2; }
    const char *lvl = getenv("PGF_LOG_LEVEL");
    logger_init(lvl ? logger_string_to_level(lvl) : LOG_LEVEL_INFO, NULL, 1);
    if (init_db() != 0) { LOG_ERROR("database unavailable"); logger_shutdown(); return 1; }
    int n = pgf_mfa_reset(argv[2]);
    if (n >= 0) LOG_INFO("reset 2FA for '%s' (%d enrollment(s) removed)", argv[2], n);
    else        LOG_ERROR("mfa-reset failed for '%s'", argv[2]);
    db_connection_pool_cleanup();
    logger_shutdown();
    return n >= 0 ? 0 : 1;
}

/* `pgforge unlock <email>` — clear an account lockout / reset its failure count
 * (admin recovery when a user is locked out). */
static int run_unlock(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: pgforge unlock <email>\n"); return 2; }
    const char *lvl = getenv("PGF_LOG_LEVEL");
    logger_init(lvl ? logger_string_to_level(lvl) : LOG_LEVEL_INFO, NULL, 1);
    if (init_db() != 0) { LOG_ERROR("database unavailable"); logger_shutdown(); return 1; }
    int n = pgf_auth_unlock(argv[2]);
    if (n > 0)       LOG_INFO("unlocked '%s'", argv[2]);
    else if (n == 0) LOG_WARN("no such user '%s'", argv[2]);
    else             LOG_ERROR("unlock failed for '%s'", argv[2]);
    db_connection_pool_cleanup();
    logger_shutdown();
    return n >= 0 ? 0 : 1;
}

/* `pgforge send-test-mail <to>` — verify the SMTP configuration by sending a test
 * message. No DB needed; reads PGF_SMTP_* / PGF_MAIL_* from the environment. */
static int run_send_test_mail(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: pgforge send-test-mail <to-address>\n"); return 2; }
    const char *lvl = getenv("PGF_LOG_LEVEL");
    logger_init(lvl ? logger_string_to_level(lvl) : LOG_LEVEL_INFO, NULL, 1);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pgf_mailer_init();
    int rc = 1;
    if (!pgf_mail_enabled()) {
        LOG_ERROR("send-test-mail: SMTP not configured (set PGF_SMTP_URL and PGF_MAIL_FROM)");
    } else if (pgf_mail_send(argv[2], "pgforge test email",
                             "This is a test message from pgforge. "
                             "If you received it, your SMTP configuration works.\n") == 0) {
        LOG_INFO("send-test-mail: sent to %s", argv[2]);
        rc = 0;
    } else {
        LOG_ERROR("send-test-mail: failed to send to %s", argv[2]);
    }
    curl_global_cleanup();
    logger_shutdown();
    return rc;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);   /* line-buffer logs even when redirected */

    if (argc >= 2 && strcmp(argv[1], "migrate") == 0)
        return run_migrate_command(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "export-tenant") == 0)
        return run_export_tenant(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "tenancy-protect") == 0)
        return run_tenancy_protect_command();
    if (argc >= 2 && strcmp(argv[1], "create-platform-admin") == 0)
        return run_create_platform_admin(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "create-tenant") == 0)
        return run_create_tenant(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "suspend-tenant") == 0)
        return run_set_tenant_active(argc, argv, 0);
    if (argc >= 2 && strcmp(argv[1], "resume-tenant") == 0)
        return run_set_tenant_active(argc, argv, 1);
    if (argc >= 2 && strcmp(argv[1], "revoke-sessions") == 0)
        return run_revoke_sessions(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "mfa-reset") == 0)
        return run_mfa_reset(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "unlock") == 0)
        return run_unlock(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "send-test-mail") == 0)
        return run_send_test_mail(argc, argv);

    int port = env_int("PGF_PORT", 8080);
    const char *lvl = getenv("PGF_LOG_LEVEL");
    log_level_t log_level = lvl ? logger_string_to_level(lvl) : LOG_LEVEL_INFO;

    if (logger_init(log_level, NULL, 1) != 0) {
        fprintf(stderr, "failed to init logger\n");
        return 1;
    }
    LOG_INFO("=== pgforge " PGF_VERSION " ===");
    LOG_INFO("log level: %s", logger_level_to_string(log_level));

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    if (pgf_crypto_init() != 0) {
        LOG_ERROR("failed to init libsodium");
        logger_shutdown();
        return 1;
    }
    pgf_auth_init();   /* precompute the login decoy hash */
    pgf_metrics_init(PGF_VERSION);            /* /metrics registry: start time + version */
    pgf_metrics_set_gauges(metrics_gauges);   /* live gauges sampled at scrape time */
    /* Opt-in session cache: PGF_SESSION_CACHE_TTL>0 skips the per-request auth DB
     * hit for up to TTL seconds (logout still evicts immediately). 0 = always DB. */
    int sc_ttl = env_int("PGF_SESSION_CACHE_TTL", 0);
    pgf_session_cache_init(sc_ttl);
    if (sc_ttl > 0) {
        LOG_INFO("session cache: %ds TTL", sc_ttl);
        /* M-1/M-2: the cache is per-process. This instance evicts on its own
         * logout/reset/revoke, but an out-of-band change (revoke-sessions CLI, a
         * role change, tenant suspend/reassign run elsewhere) is only seen here
         * after the entry's TTL expires. So token revocation, role, active-state
         * and tenant changes have a propagation latency of up to this TTL across
         * the fleet — keep it short if you need prompt revocation. */
        LOG_WARN("session cache enabled (%ds): out-of-band revocation / role / tenant "
                 "changes take effect within %ds on this instance", sc_ttl, sc_ttl);
    }
    /* Two independent throttles. PGF_AUTH_RATELIMIT="N/W" (default 10/60) guards the
     * expensive auth endpoints per IP. PGF_API_RATELIMIT="N/W" (default off) throttles
     * the data API + RPC, keyed by user id when authenticated, else IP. "0" disables. */
    {
        int rl_n = 10, rl_w = 60;
        sscanf(env_str("PGF_AUTH_RATELIMIT", "10/60"), "%d/%d", &rl_n, &rl_w);
        g_auth_rl = pgf_ratelimit_create(rl_n, rl_w);
        if (g_auth_rl) LOG_INFO("auth rate limit: %d req / %ds per IP", rl_n, rl_w);

        int api_n = 0, api_w = 60;
        sscanf(env_str("PGF_API_RATELIMIT", "0/60"), "%d/%d", &api_n, &api_w);
        g_api_rl = pgf_ratelimit_create(api_n, api_w);
        if (g_api_rl) LOG_INFO("api rate limit: %d req / %ds per caller", api_n, api_w);

        pgf_http_set_rate_limiters(g_auth_rl, g_api_rl);
    }
    /* Per-account login lockout (opt-in). PGF_AUTH_LOCKOUT="N/W": after N failed
     * password logins within W seconds, lock the account for W seconds. Unset/"0"
     * disables it (the per-IP rate limit is the primary defense). */
    {
        int lk_n = 0, lk_w = 0;
        sscanf(env_str("PGF_AUTH_LOCKOUT", "0"), "%d/%d", &lk_n, &lk_w);
        pgf_auth_set_lockout(lk_n, lk_w);
        if (lk_n > 0 && lk_w > 0) LOG_INFO("account lockout: %d fails / %ds per account", lk_n, lk_w);
    }
    /* TOTP 2FA: PGF_MFA=optional enables it (default off = fully inert). Per-user:
     * only users with a confirmed enrollment ever see the second step. */
    {
        const char *m = env_str("PGF_MFA", "off");
        int mode = !strcmp(m, "optional") ? PGF_MFA_MODE_OPTIONAL : PGF_MFA_MODE_OFF;
        pgf_mfa_set_mode(mode);
        if (mode != PGF_MFA_MODE_OFF) LOG_INFO("two-factor auth (TOTP): %s", m);
    }
    /* OIDC sign-in: PGF_OAUTH_PROVIDERS=google,apple,... (+ per-provider client id).
     * Off when unset. libcurl is initialised once here for the JWKS fetches. */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pgf_oauth_init();
    /* Outbound email (SMTP via libcurl): inert unless PGF_SMTP_URL + PGF_MAIL_FROM
     * are set. Enables password reset / verification once those land. */
    pgf_mailer_init();
    /* CORS: PGF_CORS_ORIGINS=<comma list>|* lets browser SPAs on other origins call
     * the API. Off (no CORS headers) when unset. */
    pgf_cors_init();
    if (pgf_cors_enabled()) LOG_INFO("CORS enabled for %s", env_str("PGF_CORS_ORIGINS", ""));
    /* Cap request bodies before the JSON parser sees them (CPU/memory DoS guard). */
    pgf_http_set_max_body((size_t)env_int("PGF_MAX_BODY", 1024 * 1024));

    pgf_catalog_t *catalog = NULL;
    bool db_ready = (init_db() == 0);
    if (db_ready) {
        LOG_INFO("database pool ready");
        pgf_tenancy_init();   /* PGF_TENANT_COLUMN -> pooled mode (read before auto-migrate) */
        const char *am = getenv("PGF_AUTO_MIGRATE");
        bool auto_migrate = am && (*am == '1' || *am == 't' || *am == 'T' || *am == 'y' || *am == 'Y');
        /* Seeding writes to pgf_users, which migration 001 creates — so if seeding
         * is requested we must migrate first (else the seed silently fails on a
         * fresh DB). Auto-migrate also covers the no-seed case. */
        bool want_seed = getenv("PGF_SEED_ADMIN") || getenv("PGF_SEED_USERS");
        if (auto_migrate || want_seed) {
            int applied = 0;
            int with_tenancy = pgf_tenancy_column() != NULL;   /* pooled -> apply tenancy schema */
            if (pgf_migrate_run(0, with_tenancy, &applied) == 0) LOG_INFO("migrate: %d applied on boot", applied);
            else LOG_ERROR("boot migrate failed — seeding/serving may not work");
        }
        maybe_seed_admin();
        maybe_seed_users();
        pgf_policy_init(getenv("PGF_POLICY_FILE"));   /* NULL -> built-in role defaults */
        catalog = pgf_catalog_build();
        pgf_catalog_set_active(catalog);
        pgf_rpc_audit_security_definer();   /* H-4: warn on SECURITY DEFINER RPCs */
    } else {
        LOG_WARN("database unavailable — auth opcodes will return errors");
    }

    /* Dispatcher with CPU + DB worker pools (DB pool is used once the engine lands). */
    opcode_pool_config_t pools[] = {
        {POOL_CPU, 2, 1000, "cpu"},
        {POOL_DB,  4, 1000, "db"},
    };
    g_dispatcher = opcode_dispatcher_create(pools, 2);
    if (!g_dispatcher) {
        LOG_ERROR("failed to create dispatcher");
        logger_shutdown();
        return 1;
    }
    uint32_t reg = opcode_dispatcher_register_bulk(g_dispatcher, HANDLERS, HANDLER_COUNT);
    LOG_INFO("registered %u/%zu handlers", reg, HANDLER_COUNT);

    pgf_realtime_init(rt_push, pgf_api_rt_recheck_member);   /* push framer + VIA re-authz (M-5) */

    /* WebSocket listener. */
    ws_config_t cfg = {0};
    cfg.port                = (uint16_t)port;
    cfg.thread_count        = 4;
    cfg.max_connections     = 10000;
    cfg.max_message_size    = PGF_MAX_PAYLOAD;
    cfg.small_buffer_count  = 1024;
    cfg.medium_buffer_count = 512;
    cfg.large_buffer_count  = 128;
    cfg.enable_keepalive    = true;
    cfg.enable_nodelay      = true;
    cfg.backlog             = 512;
    /* Slowloris guard: portico reaps connections still mid-handshake / mid-headers
     * after this many seconds (PGF_HEADER_TIMEOUT, default 15). */
    cfg.handshake_timeout   = (uint32_t)env_int("PGF_HEADER_TIMEOUT", 15);

    /* TLS: set PGF_TLS_CERT + PGF_TLS_KEY (PEM paths) to serve HTTPS/WSS directly
     * — no nginx needed. Both must be set; portico fails closed otherwise. */
    cfg.tls_cert_file = getenv("PGF_TLS_CERT");
    cfg.tls_key_file  = getenv("PGF_TLS_KEY");
    /* Behind a reverse proxy you control, set PGF_TRUST_PROXY=1 so the client IP
     * (for rate limiting / audit) comes from X-Real-IP / X-Forwarded-For. */
    cfg.trust_proxy   = env_int("PGF_TRUST_PROXY", 0) != 0;
    bool tls_on       = cfg.tls_cert_file && cfg.tls_key_file;

    g_server = ws_server_create(&cfg);
    if (!g_server) {
        LOG_ERROR("failed to create WebSocket server");
        opcode_dispatcher_destroy(g_dispatcher);
        logger_shutdown();
        return 1;
    }

    ws_callbacks_t cb = {0};
    cb.on_connect        = on_connect;
    cb.on_disconnect     = on_disconnect;
    cb.on_binary_message = on_binary_message;
    cb.on_http_request   = pgf_http_router;   /* REST front door (same engine) */

    if (ws_server_start(g_server, &cb) != 0) {
        LOG_ERROR("failed to start WebSocket server");
        ws_server_destroy(g_server);
        opcode_dispatcher_destroy(g_dispatcher);
        logger_shutdown();
        return 1;
    }

    LOG_INFO("%s + %s on %s://0.0.0.0:%d/  (admin UI at /, REST under /api)",
             tls_on ? "HTTPS" : "HTTP", tls_on ? "WSS" : "WS",
             tls_on ? "https" : "http", port);
    LOG_INFO("ready. ctrl-c to stop.");

    while (g_running) pause();

    LOG_INFO("shutting down...");
    ws_server_destroy(g_server);
    pgf_realtime_cleanup();
    opcode_dispatcher_destroy(g_dispatcher);
    pgf_catalog_set_active(NULL);
    pgf_catalog_free(catalog);
    pgf_session_cache_cleanup();
    pgf_ratelimit_destroy(g_auth_rl);
    pgf_ratelimit_destroy(g_api_rl);
    pgf_oauth_cleanup();
    curl_global_cleanup();
    pgf_policy_cleanup();
    if (db_ready) db_connection_pool_cleanup();
    logger_shutdown();
    return 0;
}
