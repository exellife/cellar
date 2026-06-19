/* cellar — embedded static web assets (admin UI), generated from web/. */
#ifndef CEL_WEB_ASSETS_H
#define CEL_WEB_ASSETS_H

typedef struct {
    const char          *path;   /* request path, e.g. "/" or "/app.js" */
    const unsigned char *data;
    unsigned             len;
    const char          *ctype;  /* Content-Type */
} cel_asset_t;

extern const cel_asset_t CEL_ASSETS[];
extern const int         CEL_ASSETS_COUNT;

/* Find an embedded asset by exact request path; NULL if none. */
const cel_asset_t *cel_asset_find(const char *path);

#endif /* CEL_WEB_ASSETS_H */
