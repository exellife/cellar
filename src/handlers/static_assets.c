#include "web_assets.h"
#include <string.h>

const cel_asset_t *cel_asset_find(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < CEL_ASSETS_COUNT; i++)
        if (strcmp(CEL_ASSETS[i].path, path) == 0) return &CEL_ASSETS[i];
    return NULL;
}
