/* ============================================================================
 * cellar — media upload/serve handlers (E1.1 / E1.2). See media.h.
 * ============================================================================ */
#include "media.h"

#include "core/blob_store.h"
#include "core/image_proc.h"
#include "engine/cel_apps.h"

#include <cjson/cJSON.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEDIA_MAX_DEFAULT (8u * 1024 * 1024)   /* 8 MiB wire cap (< portico 16) */
#define MEDIA_MAX_PIXELS  (25L * 1000 * 1000)  /* 25 MP decoded-area cap (~75 MB RGB) */

static size_t g_media_max = 0;
void cel_media_set_max(size_t bytes) { g_media_max = bytes; }
static size_t media_cap(void) { return g_media_max ? g_media_max : MEDIA_MAX_DEFAULT; }

/* The canonical variant set. `full` is the display image (downscaled to fit a
 * box); `thumb` is the grid/card thumbnail. Both re-encoded to JPEG. */
typedef struct { const char *name; int max_dim; int quality; } variant_t;
static const variant_t VARIANTS[] = {
    { "full",  1600, 85 },
    { "thumb",  320, 80 },
};
static const size_t NVARIANTS = sizeof VARIANTS / sizeof VARIANTS[0];

static const variant_t *variant_by_name(const char *name) {
    for (size_t i = 0; i < NVARIANTS; i++)
        if (strcmp(VARIANTS[i].name, name) == 0) return &VARIANTS[i];
    return NULL;
}

/* ---- helpers ------------------------------------------------------------- */

static int json_error(portico_response_t *res, int status, const char *msg) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "error");
    cJSON_AddStringToObject(o, "message", msg);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    portico_res_status(res, status);
    if (s) { portico_res_body(res, s, strlen(s), "application/json"); free(s); }
    return status;
}

int cel_media_id_valid(const char *id) {
    if (!id) return 0;
    size_t n = strlen(id);
    if (n != 32) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

/* Mint a 32-hex-char id from 16 CSPRNG bytes. */
static int gen_id(char out[33]) {
    unsigned char b[16];
    if (RAND_bytes(b, sizeof b) != 1) return -1;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[i*2] = hex[b[i] >> 4]; out[i*2+1] = hex[b[i] & 0xf]; }
    out[32] = '\0';
    return 0;
}

/* Blob key for an id+variant: "<ab>/<cd>/<id>/<variant>.jpg" (2-level fan-out). */
static void media_key(char *buf, size_t cap, const char *id, const char *variant) {
    snprintf(buf, cap, "%c%c/%c%c/%s/%s.jpg", id[0], id[1], id[2], id[3], id, variant);
}

/* Open the current app's media store (rooted at <bundle_dir>/media). */
static int open_store(blob_store_t *bs) {
    const cel_app_t *app = cel_apps_current();
    const char *dir = (app && app->bundle_dir[0]) ? app->bundle_dir : ".";
    char root[1100];
    snprintf(root, sizeof root, "%s/media", dir);
    return blob_store_disk_open(root, bs);
}

/* ---- E1.1: upload -------------------------------------------------------- */

int cel_media_upload(const portico_request_t *req, portico_response_t *res,
                     const cel_identity_t *who) {
    if (!who->authenticated) return json_error(res, 401, "authentication required");
    if (req->body_len == 0)  return json_error(res, 400, "empty body");
    if (req->body_len > media_cap()) return json_error(res, 413, "image too large");

    img_limits_t lim; image_limits_default(&lim);
    lim.max_bytes = media_cap();
    /* Cap the DECODED area too (not just the wire bytes): an 8 MiB highly-
     * compressed image could otherwise declare ~50 MP and blow up to ~150 MB of
     * RGB. MEDIA_MAX_PIXELS keeps a single decode bounded (~75 MB at 25 MP) — far
     * more than any phone photo needs. */
    lim.max_pixels = MEDIA_MAX_PIXELS;

    img_info_t info;
    if (image_validate(req->body, req->body_len, &lim, &info) != IMG_OK)
        return json_error(res, 400, "unsupported or invalid image");

    /* Decode ONCE; every variant is encoded from this shared RGB buffer. */
    unsigned char *rgb = NULL; int dw = 0, dh = 0;
    if (image_decode_rgb(req->body, req->body_len, &lim, &rgb, &dw, &dh) != IMG_OK)
        return json_error(res, 400, "unsupported or invalid image");

    char id[33];
    if (gen_id(id) != 0) { image_free_rgb(rgb); return json_error(res, 500, "id generation failed"); }

    blob_store_t bs;
    if (open_store(&bs) != BLOB_OK) { image_free_rgb(rgb); return json_error(res, 500, "storage unavailable"); }

    int err_code = 0; const char *err_msg = NULL;
    size_t stored = 0;                       /* variants successfully written */
    for (size_t i = 0; i < NVARIANTS; i++) {
        void *out = NULL; size_t outn = 0;
        if (image_encode_jpeg(rgb, dw, dh, VARIANTS[i].max_dim, VARIANTS[i].quality, &out, &outn) != IMG_OK) {
            err_code = 400; err_msg = "image processing failed"; break;
        }
        char key[160];
        media_key(key, sizeof key, id, VARIANTS[i].name);
        int pr = bs.put(bs.ctx, key, out, outn, "image/jpeg");
        free(out);
        if (pr != BLOB_OK) { err_code = 500; err_msg = "store failed"; break; }
        stored++;
    }
    image_free_rgb(rgb);
    if (err_code) {
        /* roll back: the id is never returned on failure, so any variants already
         * written would be permanently orphaned — delete them. */
        for (size_t j = 0; j < stored; j++) {
            char key[160];
            media_key(key, sizeof key, id, VARIANTS[j].name);
            bs.del(bs.ctx, key);
        }
        bs.destroy(bs.ctx);
        return json_error(res, err_code, err_msg);
    }
    bs.destroy(bs.ctx);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddNumberToObject(o, "width", info.width);
    cJSON_AddNumberToObject(o, "height", info.height);
    cJSON *vs = cJSON_AddArrayToObject(o, "variants");
    for (size_t i = 0; i < NVARIANTS; i++) cJSON_AddItemToArray(vs, cJSON_CreateString(VARIANTS[i].name));

    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    portico_res_status(res, 201);
    if (s) { portico_res_body(res, s, strlen(s), "application/json"); free(s); }
    return 201;
}

/* ---- E1.2: serve --------------------------------------------------------- */

int cel_media_serve(const portico_request_t *req, portico_response_t *res,
                    const char *id, const char *variant) {
    (void)req;
    if (!cel_media_id_valid(id) || !variant_by_name(variant))
        return json_error(res, 404, "not found");

    char key[160];
    media_key(key, sizeof key, id, variant);

    blob_store_t bs;
    if (open_store(&bs) != BLOB_OK) return json_error(res, 500, "storage unavailable");

    /* An adapter that serves via URL (S3/CDN) returns one; redirect to it. The
     * disk adapter returns NULL → we stream the bytes ourselves. */
    char *url = bs.url(bs.ctx, key);
    if (url) {
        portico_res_status(res, 302);
        portico_res_header(res, "Location", url);
        portico_res_body(res, "", 0, "text/plain");
        free(url);
        bs.destroy(bs.ctx);
        return 302;
    }

    blob_obj_t obj;
    int g = bs.get(bs.ctx, key, &obj);
    bs.destroy(bs.ctx);
    if (g == BLOB_ENOTFOUND) return json_error(res, 404, "not found");
    if (g != BLOB_OK)        return json_error(res, 500, "read failed");

    /* Ids are unique per upload and variants immutable → cache hard. */
    portico_res_status(res, 200);
    portico_res_header(res, "Cache-Control", "public, max-age=31536000, immutable");
    portico_res_body(res, obj.data, obj.len,
                     obj.content_type[0] ? obj.content_type : "image/jpeg");
    blob_obj_free(&obj);
    return 200;
}
