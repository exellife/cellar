/* cellar — per-app hook VM cache (design §9). See cel_hook_state.h. */
#include "cel_hook_state.h"
#include "cel_hooks.h"
#include "logger.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- per-app, shared across threads --------------------------------------- */
struct cel_hook_app {
    char            path[1024];   /* <bundle>/hooks.lua */
    pthread_mutex_t mtx;          /* guards mtime_ns + generation (stat only) */
    long long       mtime_ns;     /* last observed mtime; -2 = never checked, -1 = absent */
    unsigned        generation;   /* bumped whenever mtime_ns changes */
};

/* ---- this thread's loaded state per app (lock-free; thread-owned) ---------- */
typedef struct thread_entry {
    cel_hook_app_t      *app;
    cel_lua_t           *state;       /* may be NULL (no/failed hooks) */
    unsigned             gen_loaded;  /* the app generation this state was built at */
    struct thread_entry *next;
} thread_entry_t;
static __thread thread_entry_t *t_states = NULL;

/* Depth of hook pcalls currently executing on this thread (audit #1). While > 0,
 * some cached lua_State is live on the C stack and must not be closed for a
 * reload — cel_hook_app_state defers the reload instead. */
static __thread int t_hook_depth = 0;

void cel_hook_exec_begin(void) { t_hook_depth++; }
void cel_hook_exec_end(void)   { if (t_hook_depth > 0) t_hook_depth--; }
int  cel_hook_executing(void)  { return t_hook_depth > 0; }

cel_hook_app_t *cel_hook_app_create(const char *bundle_dir) {
    cel_hook_app_t *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    snprintf(a->path, sizeof a->path, "%s/hooks.lua", bundle_dir ? bundle_dir : ".");
    pthread_mutex_init(&a->mtx, NULL);
    a->mtime_ns   = -2;   /* force a check on first state() */
    a->generation = 0;
    return a;
}

void cel_hook_app_destroy(cel_hook_app_t *a) {
    if (!a) return;
    pthread_mutex_destroy(&a->mtx);
    free(a);
}

/* mtime of `path` in nanoseconds, or -1 if it can't be stat'd. */
static long long file_mtime_ns(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

/* Read a whole file into a NUL-terminated malloc'd buffer, or NULL on error. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* Build a fresh state: prelude + hooks.lua. NULL if there is no hooks.lua or it
 * failed to compile (logged). */
static cel_lua_t *load_state(const char *path) {
    char *src = read_file(path);
    if (!src) return NULL;   /* no hooks.lua — hooks no-op */

    char err[256];
    cel_lua_t *L = cel_lua_open();
    if (!L) { free(src); LOG_ERROR("[hook] out of memory opening state for %s", path); return NULL; }
    if (cel_hooks_install(L, err, sizeof err) != 0) {
        LOG_ERROR("[hook] prelude install failed for %s: %s", path, err);
        cel_lua_close(L); free(src); return NULL;
    }
    if (cel_lua_dostring(L, src, err, sizeof err) != 0) {
        LOG_ERROR("[hook] %s failed to load: %s", path, err);
        cel_lua_close(L); free(src); return NULL;
    }
    free(src);
    LOG_INFO("[hook] loaded %s", path);
    return L;
}

/* Current app generation, bumping it (under the per-app lock) if hooks.lua's
 * mtime changed since we last looked. The lock is held only for the stat. */
static unsigned current_generation(cel_hook_app_t *a) {
    pthread_mutex_lock(&a->mtx);
    long long now = file_mtime_ns(a->path);
    if (now != a->mtime_ns) { a->mtime_ns = now; a->generation++; }
    unsigned g = a->generation;
    pthread_mutex_unlock(&a->mtx);
    return g;
}

static thread_entry_t *find_entry(cel_hook_app_t *a) {
    for (thread_entry_t *e = t_states; e; e = e->next)
        if (e->app == a) return e;
    return NULL;
}

cel_lua_t *cel_hook_app_state(cel_hook_app_t *a) {
    if (!a) return NULL;
    unsigned g = current_generation(a);

    thread_entry_t *e = find_entry(a);
    if (e && e->gen_loaded == g) return e->state;   /* fast path: up to date */

    /* A hook is executing on this thread (this call is reentrant — e.g. a
     * before()/rpc/job hook called cellar.rt_emit, which re-enters here via the
     * on_realtime filter). This thread's lua_State is live on the C stack below,
     * so closing it to hot-reload would free the VM mid-pcall → use-after-free
     * (audit 2026-08 #1). Defer the reload: hand back the loaded state; the new
     * hooks.lua takes effect on the next top-level acquisition. gen_loaded is left
     * unchanged so the reload still happens once we're no longer executing. */
    if (e && e->state && cel_hook_executing()) return e->state;

    /* (re)load this thread's state for the app at generation g */
    if (!e) {
        e = calloc(1, sizeof *e);
        if (!e) return NULL;
        e->app = a;
        e->next = t_states;
        t_states = e;
    } else if (e->state) {
        cel_lua_close(e->state);   /* drop the stale state before reloading */
        e->state = NULL;
    }
    e->state = load_state(a->path);
    e->gen_loaded = g;
    return e->state;
}

void cel_hook_state_thread_cleanup(void) {
    thread_entry_t *e = t_states;
    while (e) {
        thread_entry_t *next = e->next;
        if (e->state) cel_lua_close(e->state);
        free(e);
        e = next;
    }
    t_states = NULL;
}
