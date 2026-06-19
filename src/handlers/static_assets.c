#include "web_assets.h"
#include <string.h>

const pgf_asset_t *pgf_asset_find(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < PGF_ASSETS_COUNT; i++)
        if (strcmp(PGF_ASSETS[i].path, path) == 0) return &PGF_ASSETS[i];
    return NULL;
}
