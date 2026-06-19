/* pgforge — embedded static web assets (admin UI), generated from web/. */
#ifndef PGF_WEB_ASSETS_H
#define PGF_WEB_ASSETS_H

typedef struct {
    const char          *path;   /* request path, e.g. "/" or "/app.js" */
    const unsigned char *data;
    unsigned             len;
    const char          *ctype;  /* Content-Type */
} pgf_asset_t;

extern const pgf_asset_t PGF_ASSETS[];
extern const int         PGF_ASSETS_COUNT;

/* Find an embedded asset by exact request path; NULL if none. */
const pgf_asset_t *pgf_asset_find(const char *path);

#endif /* PGF_WEB_ASSETS_H */
