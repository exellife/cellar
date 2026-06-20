/* ============================================================================
 * cellar — server entry point (Phase 0: boot + ping/echo)
 *
 * Boots the platform that the schema-driven engine will plug into:
 *   logger -> opcode dispatcher (thread pools) -> WebSocket listener (wslib).
 *
 * Incoming binary frames are parsed into the cel_message_t header, wrapped in an
 * opcode_context, and dispatched by opcode. Generic data-layer handlers (DB_*)
 * land here in later phases; for now only the system ops are registered.
 * ============================================================================ */
#include "wslib.h"
#include "opcode_dispatcher.h"
#include "logger.h"
#include "core/protocol.h"
#include "core/password.h"
#include "core/auth.h"
#include "core/auth_schema.h"
#include "core/session_cache.h"
#include "core/rate_limit.h"
#include "core/metrics.h"
#include "core/mfa.h"
#include "core/oauth.h"
#include "core/mailer.h"
#include "core/cors.h"

#include <curl/curl.h>
#include "engine/schema_catalog.h"
#include "engine/policy.h"
#include "engine/api.h"   /* cel_rpc_audit_security_definer, cel_api_rt_recheck_member */
#include "engine/cel_apps.h"
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

#define CEL_VERSION "0.1.0"
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <arpa/inet.h>   /* htons/htonl/ntohs/ntohl */

/* ---- global state ---------------------------------------------------------- */
static ws_server_t         *g_server     = NULL;
static opcode_dispatcher_t *g_dispatcher = NULL;
static volatile sig_atomic_t g_running   = 1;
static cel_ratelimit_t     *g_auth_rl    = NULL;   /* auth-endpoint throttle */
static cel_ratelimit_t     *g_api_rl     = NULL;   /* data-API throttle */

/* ---- response helpers ------------------------------------------------------ */

/* Frame `payload` with the 8-byte header (response bit set) and send it. */
static void cel_send(int fd, uint8_t opcode, uint8_t flags, uint16_t message_id,
                     const void *payload, size_t payload_size) {
    size_t total = CEL_HEADER_SIZE + payload_size;
    uint8_t *buf = malloc(total);
    if (!buf) return;

    cel_message_t *msg = (cel_message_t *)buf;
    msg->opcode         = opcode | CEL_FLAG_RESPONSE;
    msg->flags          = flags;
    msg->message_id     = htons(message_id);
    msg->payload_length = htonl((uint32_t)payload_size);
    if (payload_size) memcpy(msg->payload, payload, payload_size);

    ws_send_binary(g_server, fd, buf, total);
    free(buf);
}

static void cel_send_error(int fd, uint8_t opcode, uint16_t message_id,
                           const char *why) {
    cel_send(fd, opcode, CEL_FLAG_ERROR, message_id, why, strlen(why));
}

/* Realtime push: frame a CHANGE as a server-initiated message (no RESPONSE flag,
 * message_id 0) and send it. Registered with the realtime module at startup. */
static void rt_push(int fd, const char *json, size_t len) {
    size_t total = CEL_HEADER_SIZE + len;
    uint8_t *buf = malloc(total);
    if (!buf) return;
    cel_message_t *msg = (cel_message_t *)buf;
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
    static const char info[] = "{\"name\":\"cellar\",\"version\":\"" CEL_VERSION "\"}";
    return set_response(ctx, info, sizeof(info) - 1);
}

static const opcode_handler_descriptor_t HANDLERS[] = {
    /* system (synchronous) */
    {OP_PING,           handle_ping,                POOL_INLINE, "ping"},
    {OP_ECHO,           handle_echo,                POOL_INLINE, "echo"},
    {OP_SERVER_INFO,    handle_server_info,         POOL_INLINE, "server_info"},
    /* auth (DB-backed, run on the DB pool) */
    {OP_LOGIN,          cel_handle_login,           POOL_DB,     "login"},
    {OP_VERIFY_SESSION, cel_handle_verify_session,  POOL_DB,     "verify_session"},
    {OP_LOGOUT,         cel_handle_logout,          POOL_DB,     "logout"},
    /* realtime: subscribe verifies token + access rules (DB); unsubscribe is local */
    {OP_SUBSCRIBE,      cel_handle_subscribe,       POOL_DB,     "subscribe"},
    {OP_UNSUBSCRIBE,    cel_handle_unsubscribe,     POOL_INLINE, "unsubscribe"},
    /* schema-driven data layer (schema now verifies a token -> DB pool) */
    {OP_DB_SCHEMA,      cel_handle_db_schema,       POOL_DB,     "db_schema"},
    {OP_DB_LIST,        cel_handle_db_list,         POOL_DB,     "db_list"},
    {OP_DB_GET,         cel_handle_db_get,          POOL_DB,     "db_get"},
    {OP_DB_CREATE,      cel_handle_db_create,       POOL_DB,     "db_create"},
    {OP_DB_UPDATE,      cel_handle_db_update,       POOL_DB,     "db_update"},
    {OP_DB_DELETE,      cel_handle_db_delete,       POOL_DB,     "db_delete"},
    {OP_DB_RPC,         cel_handle_db_rpc,          POOL_DB,     "db_rpc"},
    {OP_DB_QUERY,       cel_handle_db_query,        POOL_DB,     "db_query"},
};
#define HANDLER_COUNT (sizeof(HANDLERS) / sizeof(HANDLERS[0]))

/* ---- async completion (for POOL_CPU / POOL_DB handlers, used later) -------- */
static void async_completion(opcode_context_t *ctx, const void *data,
                             size_t size, void *user_data) {
    (void)data; (void)user_data;
    int fd = (int)(intptr_t)ctx->user_data;
    if (ctx->response_data && ctx->response_size > 0) {
        uint8_t flags = (ctx->flags & CEL_FLAG_ERROR);
        cel_send(fd, (uint8_t)ctx->opcode, flags, (uint16_t)ctx->message_id,
                 ctx->response_data, ctx->response_size);
    } else {
        cel_send_error(fd, (uint8_t)ctx->opcode, (uint16_t)ctx->message_id,
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
    cel_realtime_drop_conn(fd);   /* tear down this connection's subscriptions */
    LOG_DEBUG("connection closed: fd=%d", fd);
}

/* Sampled at /metrics scrape time: pull live gauges from the subsystems that own
 * them (keeps core/metrics a dependency-free leaf). Active connections come from
 * portico (mechanism); the rest from our own subsystems. */
static void metrics_gauges(cel_gauges_t *g) {
    g->db_pool_size = 0; g->db_pool_in_use = 0;   /* no shared pool — one SQLite file per app */
    g->realtime_subscriptions = cel_realtime_count();
    g->active_connections     = portico_active_connections(g_server);
}

static int on_binary_message(int fd, const void *data, size_t len, void *user_data) {
    /* user_data is the connection's Host header (set by portico at handshake). */

    if (len < CEL_HEADER_SIZE) {
        LOG_WARN("short frame (%zu bytes) from fd=%d", len, fd);
        return -1;
    }
    const cel_message_t *msg = (const cel_message_t *)data;
    uint32_t payload_len = ntohl(msg->payload_length);
    if (len != CEL_HEADER_SIZE + payload_len) {
        LOG_WARN("length mismatch (got %zu, expected %u) from fd=%d",
                 len, CEL_HEADER_SIZE + payload_len, fd);
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
    /* Route by the connection's Host (portico delivers it as the WS user_data).
     * Resolve the app here, on the connection thread, and carry it on the ctx
     * (callback_data) so the handler — which may run on a DB worker thread — can
     * bind it. NULL in multi-app mode for an unknown Host (the op then errors). */
    cel_app_t *app = cel_apps_resolve((const char *)user_data);
    opcode_context_set_callback(ctx, async_completion, app);

    handler_result_t r = opcode_dispatcher_dispatch(g_dispatcher, ctx);
    if (r == RESULT_SUCCESS && ctx->response_data) {
        cel_send(fd, msg->opcode, 0, message_id,
                 ctx->response_data, ctx->response_size);
        opcode_context_destroy(ctx);
    } else if (r == RESULT_ASYNC) {
        /* thread pool owns ctx; async_completion will fire */
    } else {
        LOG_ERROR("dispatch error for opcode 0x%02X from fd=%d", msg->opcode, fd);
        cel_send_error(fd, msg->opcode, message_id, "no handler / error");
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

/* Optional first-run seeding: CEL_SEED_ADMIN="email:password". */
static void maybe_seed_admin(void) {
    const char *spec = getenv("CEL_SEED_ADMIN");
    if (!spec || !*spec) return;
    const char *colon = strchr(spec, ':');
    if (!colon || colon == spec || !colon[1]) {
        LOG_WARN("CEL_SEED_ADMIN must be 'email:password' — skipping");
        return;
    }
    char email[256];
    size_t elen = (size_t)(colon - spec);
    if (elen >= sizeof email) { LOG_WARN("CEL_SEED_ADMIN email too long"); return; }
    memcpy(email, spec, elen);
    email[elen] = '\0';
    if (cel_auth_seed_admin(email, colon + 1) == 0)
        LOG_INFO("seeded admin user '%s'", email);
    else
        LOG_ERROR("failed to seed admin user '%s'", email);
}

/* Optional multi-user seeding: CEL_SEED_USERS="email:pass:role;email:pass:role". */
static void maybe_seed_users(void) {
    const char *spec = getenv("CEL_SEED_USERS");
    if (!spec || !*spec) return;
    char *dup = strdup(spec);
    if (!dup) return;
    char *save = NULL;
    for (char *rec = strtok_r(dup, ";", &save); rec; rec = strtok_r(NULL, ";", &save)) {
        char *c1 = strchr(rec, ':');
        char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        if (!c1 || !c2) { LOG_WARN("CEL_SEED_USERS entry must be email:pass:role"); continue; }
        *c1 = '\0'; *c2 = '\0';
        const char *em = rec, *pw = c1 + 1, *role = c2 + 1;
        if (cel_auth_seed_user(em, pw, role) == 0) LOG_INFO("seeded user '%s' (%s)", em, role);
        else                                       LOG_ERROR("failed to seed user '%s'", em);
    }
    free(dup);
}

/* Open the app's SQLite database for a one-shot admin CLI: init the registry,
 * open CEL_DATA_DB, apply the auth schema, and make it the current app so the
 * cel_auth_* / cel_mfa_* helpers below operate on it. Returns 0 when ready. */
static int cli_open_app(void) {
    const char *lvl = getenv("CEL_LOG_LEVEL");
    logger_init(lvl ? logger_string_to_level(lvl) : LOG_LEVEL_INFO, NULL, 1);
    app_db_global_init();
    app_db_t *app = app_db_get(env_str("CEL_DATA_DB", "cellar.db"));
    if (!app) { LOG_ERROR("could not open app database"); logger_shutdown(); return 1; }
    sqlite3 *c = app_db_conn_acquire(app);
    if (c) { cel_auth_schema_apply(c); app_db_conn_release(app, c); }
    app_db_set_current(app);
    return 0;
}
static int cli_done(int ok) {
    app_db_global_shutdown();
    logger_shutdown();
    return ok ? 0 : 1;
}

/* `cellar revoke-sessions <email>` — force-logout: delete every session for a
 * user (e.g. after a credential compromise). NOTE: a CLI run deletes the rows;
 * a running server re-validates within its session-cache TTL (0 = immediate). */
static int run_revoke_sessions(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: cellar revoke-sessions <email>\n"); return 2; }
    if (cli_open_app() != 0) return 1;
    int n = cel_auth_revoke_user_sessions(argv[2]);
    if (n >= 0) {
        LOG_INFO("revoked %d session(s) for '%s'", n, argv[2]);
        int sct = env_int("CEL_SESSION_CACHE_TTL", 0);   /* M-1: warn about cache latency */
        if (sct > 0)
            LOG_WARN("a running server with the session cache enabled may still honor these "
                     "tokens for up to %ds (CEL_SESSION_CACHE_TTL)", sct);
    } else        LOG_ERROR("revoke failed for '%s'", argv[2]);
    return cli_done(n >= 0);
}

/* `cellar mfa-reset <email>` — admin lockout recovery: remove a user's TOTP
 * enrollment so they can log in with just their password (and re-enroll). */
static int run_mfa_reset(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: cellar mfa-reset <email>\n"); return 2; }
    if (cli_open_app() != 0) return 1;
    int n = cel_mfa_reset(argv[2]);
    if (n >= 0) LOG_INFO("reset 2FA for '%s' (%d enrollment(s) removed)", argv[2], n);
    else        LOG_ERROR("mfa-reset failed for '%s'", argv[2]);
    return cli_done(n >= 0);
}

/* `cellar unlock <email>` — clear an account lockout / reset its failure count
 * (admin recovery when a user is locked out). */
static int run_unlock(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: cellar unlock <email>\n"); return 2; }
    if (cli_open_app() != 0) return 1;
    int n = cel_auth_unlock(argv[2]);
    if (n > 0)       LOG_INFO("unlocked '%s'", argv[2]);
    else if (n == 0) LOG_WARN("no such user '%s'", argv[2]);
    else             LOG_ERROR("unlock failed for '%s'", argv[2]);
    return cli_done(n >= 0);
}

/* `cellar send-test-mail <to>` — verify the SMTP configuration by sending a test
 * message. No DB needed; reads CEL_SMTP_* / CEL_MAIL_* from the environment. */
static int run_send_test_mail(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: cellar send-test-mail <to-address>\n"); return 2; }
    const char *lvl = getenv("CEL_LOG_LEVEL");
    logger_init(lvl ? logger_string_to_level(lvl) : LOG_LEVEL_INFO, NULL, 1);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    cel_mailer_init();
    int rc = 1;
    if (!cel_mail_enabled()) {
        LOG_ERROR("send-test-mail: SMTP not configured (set CEL_SMTP_URL and CEL_MAIL_FROM)");
    } else if (cel_mail_send(argv[2], "cellar test email",
                             "This is a test message from cellar. "
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

/* Write `content` to `path` only if it doesn't already exist (so re-provisioning
 * never clobbers a hand-edited bundle file). Returns 1 if written, 0 if it existed. */
static int write_if_absent(const char *path, const char *content) {
    struct stat st;
    if (stat(path, &st) == 0) return 0;   /* keep existing */
    FILE *f = fopen(path, "wb");
    if (!f) { LOG_WARN("provision: could not write %s: %s", path, strerror(errno)); return 0; }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return 1;
}

static const char *STARTER_INDEX =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>cellar app</title></head>\n"
    "<body>\n"
    "  <h1>It works.</h1>\n"
    "  <p>This is your app's <code>public/</code> root. Put your front-end here\n"
    "     (HTML/CSS/JS/images/fonts). It talks to the backend over\n"
    "     <code>/auth</code>, <code>/api</code>, <code>/rpc</code> and the realtime WebSocket.</p>\n"
    "</body>\n"
    "</html>\n";

static const char *STARTER_HOOKS =
    "-- hooks.lua — this app's behavior (the cellar hook contract, design §8).\n"
    "-- Every hook is optional; an absent hook is a no-op. The `cellar` table gives\n"
    "-- you cellar.query/exec (parameterized SQL on this app's db) and cellar.log.\n"
    "--\n"
    "-- function authorize(op, table, row, who)   return true end   -- extra allow gate\n"
    "-- function before(op, table, input, who)    return true end   -- validate / transform input\n"
    "-- function after(op, table, row, who)        end               -- post-commit side effects\n"
    "-- function rpc(name, args, who)             return { ok = true } end  -- POST /rpc/<name>\n"
    "-- function on_realtime(change, subscriber)  return true end   -- realtime delivery filter\n";

/* `cellar provision <host> [<admin-email> <admin-password>]` — scaffold a new app
 * bundle under CEL_APPS_DIR: create <dir>/<host>/, initialize data.db with the
 * identity schema, seed an admin (from args or CEL_SEED_ADMIN), and drop a starter
 * hooks.lua. Idempotent: re-running keeps existing data.db rows and bundle files. */
static int run_provision(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: cellar provision <host> [<admin-email> <admin-password>]\n");
        return 2;
    }
    const char *lvl = getenv("CEL_LOG_LEVEL");
    logger_init(lvl ? logger_string_to_level(lvl) : LOG_LEVEL_INFO, NULL, 1);

    const char *apps_dir = env_str("CEL_APPS_DIR", "");
    if (!*apps_dir) {
        LOG_ERROR("provision needs CEL_APPS_DIR (the multi-app bundles directory)");
        logger_shutdown(); return 1;
    }
    char host[256];
    if (cel_apps_norm_host(argv[2], host, sizeof host) != 0) {
        LOG_ERROR("invalid host '%s' (use lowercase letters, digits, '.' and '-')", argv[2]);
        logger_shutdown(); return 1;
    }

    char dir[1300];
    snprintf(dir, sizeof dir, "%s/%s", apps_dir, host);
    mkdir(apps_dir, 0755);   /* ensure the parent exists (ignore EEXIST) */
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        LOG_ERROR("provision: mkdir %s: %s", dir, strerror(errno));
        logger_shutdown(); return 1;
    }

    if (cel_crypto_init() != 0) { LOG_ERROR("provision: crypto init failed"); logger_shutdown(); return 1; }
    app_db_global_init();
    char db[1400];
    snprintf(db, sizeof db, "%s/data.db", dir);
    app_db_t *app = app_db_get(db);
    if (!app) { LOG_ERROR("provision: could not open %s", db); app_db_global_shutdown(); logger_shutdown(); return 1; }
    sqlite3 *c = app_db_conn_acquire(app);
    if (c) { cel_auth_schema_apply(c); app_db_conn_release(app, c); }
    app_db_set_current(app);   /* so cel_auth_seed_admin writes into this bundle */

    /* Seed an admin: explicit args win, else CEL_SEED_ADMIN ("email:password"). */
    if (argc >= 5) {
        if (cel_auth_seed_admin(argv[3], argv[4]) == 0) LOG_INFO("seeded admin '%s'", argv[3]);
        else { LOG_ERROR("provision: failed to seed admin '%s'", argv[3]);
               app_db_global_shutdown(); logger_shutdown(); return 1; }
    } else if (getenv("CEL_SEED_ADMIN")) {
        maybe_seed_admin();
    } else {
        LOG_WARN("no admin seeded (pass <admin-email> <admin-password> or set CEL_SEED_ADMIN); "
                 "the app has no way to log in yet");
    }

    char hooks_path[1400];
    snprintf(hooks_path, sizeof hooks_path, "%s/hooks.lua", dir);
    if (write_if_absent(hooks_path, STARTER_HOOKS)) LOG_INFO("wrote starter %s", hooks_path);

    /* public/ — the bundle's front-end root (HTML/CSS/JS/images/fonts). Part of the
     * bundle layout (design §4); per-app static serving from here is the next
     * wiring step (cel_http_router → portico_res_static with an SPA fallback). */
    char pub_dir[1400];
    snprintf(pub_dir, sizeof pub_dir, "%s/public", dir);
    mkdir(pub_dir, 0755);   /* ignore EEXIST */
    char index_path[1500];
    snprintf(index_path, sizeof index_path, "%s/index.html", pub_dir);
    if (write_if_absent(index_path, STARTER_INDEX)) LOG_INFO("wrote starter %s", index_path);

    LOG_INFO("provisioned app '%s' at %s", host, dir);
    app_db_global_shutdown();
    logger_shutdown();
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);   /* line-buffer logs even when redirected */

    if (argc >= 2 && strcmp(argv[1], "revoke-sessions") == 0)
        return run_revoke_sessions(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "mfa-reset") == 0)
        return run_mfa_reset(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "unlock") == 0)
        return run_unlock(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "send-test-mail") == 0)
        return run_send_test_mail(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "provision") == 0)
        return run_provision(argc, argv);

    int port = env_int("CEL_PORT", 8080);
    const char *lvl = getenv("CEL_LOG_LEVEL");
    log_level_t log_level = lvl ? logger_string_to_level(lvl) : LOG_LEVEL_INFO;

    if (logger_init(log_level, NULL, 1) != 0) {
        fprintf(stderr, "failed to init logger\n");
        return 1;
    }
    LOG_INFO("=== cellar " CEL_VERSION " ===");
    LOG_INFO("log level: %s", logger_level_to_string(log_level));

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    if (cel_crypto_init() != 0) {
        LOG_ERROR("failed to init libsodium");
        logger_shutdown();
        return 1;
    }
    if (cel_auth_init() != 0) {   /* precompute the login decoy hash — must succeed (L-1) */
        LOG_ERROR("failed to precompute the login decoy hash — refusing to start "
                  "(without it, login latency would reveal which emails exist)");
        logger_shutdown();
        return 1;
    }
    cel_metrics_init(CEL_VERSION);            /* /metrics registry: start time + version */
    cel_metrics_set_gauges(metrics_gauges);   /* live gauges sampled at scrape time */
    /* Opt-in session cache: CEL_SESSION_CACHE_TTL>0 skips the per-request auth DB
     * hit for up to TTL seconds (logout still evicts immediately). 0 = always DB. */
    int sc_ttl = env_int("CEL_SESSION_CACHE_TTL", 0);
    cel_session_cache_init(sc_ttl);
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
    /* Two independent throttles. CEL_AUTH_RATELIMIT="N/W" (default 10/60) guards the
     * expensive auth endpoints per IP. CEL_API_RATELIMIT="N/W" (default off) throttles
     * the data API + RPC, keyed by user id when authenticated, else IP. "0" disables. */
    {
        int rl_n = 10, rl_w = 60;
        sscanf(env_str("CEL_AUTH_RATELIMIT", "10/60"), "%d/%d", &rl_n, &rl_w);
        g_auth_rl = cel_ratelimit_create(rl_n, rl_w);
        if (g_auth_rl) LOG_INFO("auth rate limit: %d req / %ds per IP", rl_n, rl_w);

        int api_n = 0, api_w = 60;
        sscanf(env_str("CEL_API_RATELIMIT", "0/60"), "%d/%d", &api_n, &api_w);
        g_api_rl = cel_ratelimit_create(api_n, api_w);
        if (g_api_rl) LOG_INFO("api rate limit: %d req / %ds per caller", api_n, api_w);

        cel_http_set_rate_limiters(g_auth_rl, g_api_rl);
    }
    /* Per-account login lockout (opt-in). CEL_AUTH_LOCKOUT="N/W": after N failed
     * password logins within W seconds, lock the account for W seconds. Unset/"0"
     * disables it (the per-IP rate limit is the primary defense). */
    {
        int lk_n = 0, lk_w = 0;
        sscanf(env_str("CEL_AUTH_LOCKOUT", "0"), "%d/%d", &lk_n, &lk_w);
        cel_auth_set_lockout(lk_n, lk_w);
        if (lk_n > 0 && lk_w > 0) LOG_INFO("account lockout: %d fails / %ds per account", lk_n, lk_w);
    }
    /* TOTP 2FA: CEL_MFA=optional enables it (default off = fully inert). Per-user:
     * only users with a confirmed enrollment ever see the second step. */
    {
        const char *m = env_str("CEL_MFA", "off");
        int mode = !strcmp(m, "optional") ? CEL_MFA_MODE_OPTIONAL : CEL_MFA_MODE_OFF;
        cel_mfa_set_mode(mode);
        if (mode != CEL_MFA_MODE_OFF) LOG_INFO("two-factor auth (TOTP): %s", m);
    }
    /* OIDC sign-in: CEL_OAUTH_PROVIDERS=google,apple,... (+ per-provider client id).
     * Off when unset. libcurl is initialised once here for the JWKS fetches. */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    cel_oauth_init();
    /* Outbound email (SMTP via libcurl): inert unless CEL_SMTP_URL + CEL_MAIL_FROM
     * are set. Enables password reset / verification once those land. */
    cel_mailer_init();
    /* CORS: CEL_CORS_ORIGINS=<comma list>|* lets browser SPAs on other origins call
     * the API. Off (no CORS headers) when unset. */
    cel_cors_init();
    if (cel_cors_enabled()) LOG_INFO("CORS enabled for %s", env_str("CEL_CORS_ORIGINS", ""));
    /* Cap request bodies before the JSON parser sees them (CPU/memory DoS guard). */
    cel_http_set_max_body((size_t)env_int("CEL_MAX_BODY", 1024 * 1024));

    cel_policy_init(getenv("CEL_POLICY_FILE"));   /* NULL -> built-in role defaults */

    /* App registry + routing (design §4-5). CEL_APPS_DIR set → multi-app: each
     * request's Host resolves to <dir>/<host>/data.db, opened lazily and bound per
     * request (HTTP). Otherwise single-app from CEL_DATA_DB, which becomes the
     * process default (the WS path and boot-time seeding bind it). */
    app_db_global_init();
    cel_apps_init(env_str("CEL_APPS_DIR", ""), env_str("CEL_DATA_DB", "cellar.db"));

    cel_app_t *def = cel_apps_default();   /* the single app (single-app mode) */
    if (def) {
        app_db_set_default(def->db);
        cel_catalog_set_default(def->catalog);
        LOG_INFO("default app: %d table(s)", def->catalog ? def->catalog->ntables : 0);
        /* Seed admin / dev users into the default app's identity tables. */
        cel_apps_enter(def);
        maybe_seed_admin();
        maybe_seed_users();
        cel_apps_leave();
    } else {
        LOG_INFO("multi-app mode — apps open per request by Host; no boot seeding");
    }
    cel_rpc_audit_security_definer();   /* no-op on SQLite; kept for the call site */

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

    cel_realtime_init(rt_push, cel_api_rt_recheck_member);   /* push framer + VIA re-authz (M-5) */
    cel_realtime_set_filter(cel_api_rt_filter);              /* the on_realtime() delivery hook */

    /* WebSocket listener. */
    ws_config_t cfg = {0};
    cfg.port                = (uint16_t)port;
    cfg.thread_count        = 4;
    cfg.max_connections     = 10000;
    cfg.max_message_size    = CEL_MAX_PAYLOAD;
    cfg.small_buffer_count  = 1024;
    cfg.medium_buffer_count = 512;
    cfg.large_buffer_count  = 128;
    cfg.enable_keepalive    = true;
    cfg.enable_nodelay      = true;
    cfg.backlog             = 512;
    /* Slowloris guard: portico reaps connections still mid-handshake / mid-headers
     * after this many seconds (CEL_HEADER_TIMEOUT, default 15). */
    cfg.handshake_timeout   = (uint32_t)env_int("CEL_HEADER_TIMEOUT", 15);

    /* TLS: set CEL_TLS_CERT + CEL_TLS_KEY (PEM paths) to serve HTTPS/WSS directly
     * — no nginx needed. Both must be set; portico fails closed otherwise. */
    cfg.tls_cert_file = getenv("CEL_TLS_CERT");
    cfg.tls_key_file  = getenv("CEL_TLS_KEY");
    /* Behind a reverse proxy you control, set CEL_TRUST_PROXY=1 so the client IP
     * (for rate limiting / audit) comes from X-Real-IP / X-Forwarded-For. */
    cfg.trust_proxy   = env_int("CEL_TRUST_PROXY", 0) != 0;
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
    cb.on_http_request   = cel_http_router;   /* REST front door (same engine) */

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
    cel_realtime_cleanup();
    opcode_dispatcher_destroy(g_dispatcher);
    cel_apps_shutdown();          /* frees the per-app catalogs */
    cel_session_cache_cleanup();
    cel_ratelimit_destroy(g_auth_rl);
    cel_ratelimit_destroy(g_api_rl);
    cel_oauth_cleanup();
    curl_global_cleanup();
    cel_policy_cleanup();
    app_db_global_shutdown();
    logger_shutdown();
    return 0;
}
