/* cellar — session cache unit test (no DB).
 *
 * Pins the cache's contract: hit/miss, refresh, evict-on-logout, the disabled
 * (ttl<=0) mode that must behave exactly like "always hit the DB", and TTL
 * expiry. Links session_cache.c alone (cel_user_t is a plain struct from auth.h).
 */
#include "session_cache.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static void chk(const char *label, int cond) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", label);
    if (!cond) failures++;
}

static cel_user_t mkuser(const char *id, const char *role) {
    cel_user_t u; memset(&u, 0, sizeof u);
    snprintf(u.id, sizeof u.id, "%s", id);
    snprintf(u.role, sizeof u.role, "%s", role);
    snprintf(u.email, sizeof u.email, "%s@x", id);
    return u;
}

int main(void) {
    printf("session cache\n");
    cel_user_t out;

    /* ---- disabled (ttl<=0): always a miss, put is inert ---- */
    cel_session_cache_init(0);
    cel_user_t a = mkuser("u1", "editor");
    cel_session_cache_put("tok1", &a);
    chk("disabled -> miss", !cel_session_cache_get("tok1", &out));

    /* ---- enabled: put then hit, with the stored identity ---- */
    cel_session_cache_init(60);
    cel_session_cache_put("tok1", &a);
    chk("hit after put", cel_session_cache_get("tok1", &out));
    chk("hit carries id", !strcmp(out.id, "u1") && !strcmp(out.role, "editor"));
    chk("unknown token miss", !cel_session_cache_get("nope", &out));

    /* ---- refresh replaces the stored identity (e.g. role change on re-verify) ---- */
    cel_user_t a2 = mkuser("u1", "admin");
    cel_session_cache_put("tok1", &a2);
    chk("refresh updates role", cel_session_cache_get("tok1", &out) && !strcmp(out.role, "admin"));

    /* ---- evict (logout) drops it immediately ---- */
    cel_session_cache_evict("tok1");
    chk("evicted -> miss", !cel_session_cache_get("tok1", &out));

    /* ---- many entries coexist ---- */
    for (int i = 0; i < 1000; i++) {
        char t[32]; snprintf(t, sizeof t, "tok-%d", i);
        cel_user_t u = mkuser(t, "viewer");
        cel_session_cache_put(t, &u);
    }
    chk("entry 0 present",   cel_session_cache_get("tok-0", &out));
    chk("entry 999 present", cel_session_cache_get("tok-999", &out));

    /* ---- TTL expiry: a 1s TTL entry is gone after >1s ---- */
    cel_session_cache_init(1);
    cel_user_t b = mkuser("u2", "viewer");
    cel_session_cache_put("ttltok", &b);
    chk("fresh -> hit", cel_session_cache_get("ttltok", &out));
    sleep(2);
    chk("expired -> miss", !cel_session_cache_get("ttltok", &out));

    cel_session_cache_cleanup();
    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
