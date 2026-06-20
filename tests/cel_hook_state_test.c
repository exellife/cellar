/* cellar — per-app hook VM cache test (Phase 2). A bundle dir with no hooks.lua
 * yields no state (hooks no-op); a hooks.lua loads and dispatches; editing it
 * hot-reloads on the next get (mtime-driven, forced deterministically here via
 * utimensat); a broken hooks.lua caches NULL and recovers when fixed; and two
 * threads each get their own independent state. */
#define _GNU_SOURCE
#include "cel_hook_state.h"
#include "cel_hooks.h"
#include "cel_lua.h"
#include "cel_val.h"
#include <cjson/cJSON.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails = 0;
static void check(const char *name, int cond, const char *detail) {
    printf("  %-4s %-42s %s\n", cond ? "ok" : "FAIL", name, detail ? detail : "");
    if (!cond) fails++;
}

static char g_hooks[1100];   /* <dir>/hooks.lua */

static void write_hooks(const char *src, long mtime_sec) {
    FILE *f = fopen(g_hooks, "wb");
    fwrite(src, 1, strlen(src), f);
    fclose(f);
    struct timespec ts[2] = { { mtime_sec, 0 }, { mtime_sec, 0 } };
    utimensat(AT_FDCWD, g_hooks, ts, 0);   /* force a deterministic mtime */
}

/* call rpc('go') on the app's current state; returns the numeric `v` it yields,
 * or -1 if there is no state / it errored. */
static double rpc_v(cel_hook_app_t *app) {
    cel_lua_t *L = cel_hook_app_state(app);
    if (!L) return -1;
    cJSON *who = cJSON_Parse("{}");
    char err[256];
    cel_val_t *res = cel_hooks_rpc(L, "go", NULL, (cel_val_t *)who, err, sizeof err);
    double v = res ? cel_val_num(cel_val_get(res, "v")) : -1;
    if (res) cel_val_free(res);
    cJSON_Delete(who);
    return v;
}

static const char *V1  = "function rpc(name, args, who) return { v = 1 } end\n";
static const char *V2  = "function rpc(name, args, who) return { v = 2 } end\n";
static const char *BAD = "function rpc( this is not lua\n";

static void *thread_main(void *arg) {
    cel_hook_app_t *app = arg;
    /* each thread builds its OWN state for the app and dispatches on it */
    double v = rpc_v(app);
    cel_hook_state_thread_cleanup();   /* free this worker's states before exit */
    return (void *)(long)(v == 2.0 ? 1 : 0);
}

int main(void) {
    char dir[] = "/tmp/celhookXXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(g_hooks, sizeof g_hooks, "%s/hooks.lua", dir);

    /* ---- no hooks.lua → no state, hooks no-op ---- */
    cel_hook_app_t *app = cel_hook_app_create(dir);
    check("create app (no hooks.lua yet)", app != NULL, "");
    check("no hooks.lua -> NULL state", cel_hook_app_state(app) == NULL, "");

    /* ---- hooks.lua appears → loads + dispatches ---- */
    write_hooks(V1, 1000000000);
    check("hooks.lua appears -> state loads", cel_hook_app_state(app) != NULL, "");
    check("rpc dispatches on loaded state (v==1)", rpc_v(app) == 1.0, "");

    /* ---- edit hooks.lua → hot-reload on next get ---- */
    write_hooks(V2, 1000000100);   /* newer mtime */
    check("edited hooks.lua hot-reloads (v==2)", rpc_v(app) == 2.0, "");

    /* ---- unchanged file → no reload (stable, same result) ---- */
    check("no change -> still v==2", rpc_v(app) == 2.0, "");

    /* ---- a broken hooks.lua caches NULL, then recovers when fixed ---- */
    write_hooks(BAD, 1000000200);
    check("compile error -> NULL state", cel_hook_app_state(app) == NULL, "");
    check("broken state stays NULL (cached)", cel_hook_app_state(app) == NULL, "");
    write_hooks(V1, 1000000300);
    check("fixed hooks.lua recovers (v==1)", rpc_v(app) == 1.0, "");

    /* ---- per-thread independence: a worker thread builds its own state ---- */
    write_hooks(V2, 1000000400);
    check("main thread sees v==2", rpc_v(app) == 2.0, "");
    pthread_t th;
    pthread_create(&th, NULL, thread_main, app);
    void *tr = NULL;
    pthread_join(th, &tr);
    check("worker thread got its own working state", (long)tr == 1, "");

    cel_hook_state_thread_cleanup();   /* free the main thread's states */
    cel_hook_app_destroy(app);
    unlink(g_hooks);
    rmdir(dir);

    printf("\n%s  (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
