#include "cors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ORIGINS 16
#define ORIGIN_LEN  256

static char g_origins[MAX_ORIGINS][ORIGIN_LEN];
static int  g_norigins = 0;
static bool g_wildcard = false;     /* CEL_CORS_ORIGINS = "*" */
static bool g_credentials = false;

void cel_cors_init(void) {
    g_norigins = 0;
    g_wildcard = false;
    const char *list = getenv("CEL_CORS_ORIGINS");
    if (!list || !*list) return;

    if (!strcmp(list, "*")) { g_wildcard = true; }
    else {
        char *dup = strdup(list);
        if (!dup) return;
        for (char *tok = strtok(dup, ","); tok && g_norigins < MAX_ORIGINS; tok = strtok(NULL, ",")) {
            while (*tok == ' ') tok++;
            size_t n = strlen(tok);
            while (n > 0 && (tok[n - 1] == ' ' || tok[n - 1] == '/')) tok[--n] = '\0';  /* trim, drop trailing slash */
            if (*tok) snprintf(g_origins[g_norigins++], ORIGIN_LEN, "%s", tok);
        }
        free(dup);
    }
    const char *cred = getenv("CEL_CORS_CREDENTIALS");
    g_credentials = cred && (*cred == '1' || *cred == 't' || *cred == 'T' || *cred == 'y' || *cred == 'Y');
}

bool cel_cors_enabled(void) { return g_wildcard || g_norigins > 0; }

bool cel_cors_allow_credentials(void) { return g_credentials && cel_cors_enabled(); }

const char *cel_cors_allow_origin(const char *origin) {
    if (!origin || !*origin) return NULL;
    if (g_wildcard) {
        /* With credentials, "*" is invalid — echo the specific origin instead. */
        return g_credentials ? origin : "*";
    }
    for (int i = 0; i < g_norigins; i++)
        if (!strcmp(g_origins[i], origin)) return g_origins[i];   /* stable echo (not the arg) */
    return NULL;
}
