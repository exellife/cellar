/* pgforge — CORS origin-policy unit test (no DB). Drives pgf_cors_init via env. */
#include "cors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static void chk(const char *label, int cond) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", label);
    if (!cond) failures++;
}
/* allow_origin result equals `want` (NULL-safe). */
static int eq(const char *got, const char *want) {
    if (!got || !want) return got == want;
    return strcmp(got, want) == 0;
}

static void configure(const char *origins, const char *creds) {
    if (origins) setenv("PGF_CORS_ORIGINS", origins, 1); else unsetenv("PGF_CORS_ORIGINS");
    if (creds)   setenv("PGF_CORS_CREDENTIALS", creds, 1); else unsetenv("PGF_CORS_CREDENTIALS");
    pgf_cors_init();
}

int main(void) {
    printf("CORS origin policy\n");

    /* disabled by default */
    configure(NULL, NULL);
    chk("disabled when unset", !pgf_cors_enabled());
    chk("disabled -> no allow", eq(pgf_cors_allow_origin("https://a.test"), NULL));

    /* explicit allowlist */
    configure("https://a.test, https://b.test", NULL);
    chk("enabled", pgf_cors_enabled());
    chk("listed origin echoed", eq(pgf_cors_allow_origin("https://a.test"), "https://a.test"));
    chk("second listed origin", eq(pgf_cors_allow_origin("https://b.test"), "https://b.test"));
    chk("unlisted origin denied", eq(pgf_cors_allow_origin("https://evil.test"), NULL));
    chk("empty origin denied", eq(pgf_cors_allow_origin(""), NULL));
    chk("no credentials by default", !pgf_cors_allow_credentials());

    /* trailing slash is trimmed when matching */
    configure("https://a.test/", NULL);
    chk("trailing slash trimmed", eq(pgf_cors_allow_origin("https://a.test"), "https://a.test"));

    /* wildcard without credentials -> "*" */
    configure("*", NULL);
    chk("wildcard echoes *", eq(pgf_cors_allow_origin("https://anything.test"), "*"));

    /* wildcard WITH credentials -> echo the specific origin (spec: no "*" + creds) */
    configure("*", "1");
    chk("wildcard+creds echoes origin", eq(pgf_cors_allow_origin("https://anything.test"), "https://anything.test"));
    chk("credentials enabled", pgf_cors_allow_credentials());

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
