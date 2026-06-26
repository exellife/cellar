/* ============================================================================
 * cellar — image processing implementation (stb-backed).
 *
 * Declarations only here — the stb *_IMPLEMENTATION lives in stb_impl.c.
 * ============================================================================ */
#include "image_proc.h"

#include <stdlib.h>
#include <string.h>

#include "stb/stb_image.h"
#include "stb/stb_image_resize2.h"
#include "stb/stb_image_write.h"

/* Built-in defaults (used when a limits field is 0). */
#define DEF_MAX_BYTES   (30u * 1024 * 1024)   /* 30 MiB input file            */
#define DEF_MAX_DIM     20000                 /* per-axis declared dimension  */
#define DEF_MAX_PIXELS  (50L * 1000 * 1000)   /* 50 MP area (bomb guard)      */
#define DEF_JPEG_Q      82

void image_limits_default(img_limits_t *l) {
    if (!l) return;
    l->max_bytes  = DEF_MAX_BYTES;
    l->max_width  = DEF_MAX_DIM;
    l->max_height = DEF_MAX_DIM;
    l->max_pixels = DEF_MAX_PIXELS;
}

/* Resolve a limits struct, substituting defaults for any 0 field. */
static img_limits_t resolve_limits(const img_limits_t *in) {
    img_limits_t l;
    image_limits_default(&l);
    if (in) {
        if (in->max_bytes)  l.max_bytes  = in->max_bytes;
        if (in->max_width)  l.max_width  = in->max_width;
        if (in->max_height) l.max_height = in->max_height;
        if (in->max_pixels) l.max_pixels = in->max_pixels;
    }
    return l;
}

/* Sniff format from magic bytes (does not trust any extension). */
static img_format_t sniff(const unsigned char *d, size_t n) {
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return IMG_FMT_JPEG;
    static const unsigned char png[8] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
    if (n >= 8 && memcmp(d, png, 8) == 0)
        return IMG_FMT_PNG;
    return IMG_FMT_UNKNOWN;
}

int image_validate(const void *data, size_t len,
                   const img_limits_t *limits, img_info_t *info) {
    if (!data || !info) return IMG_EINVAL;
    memset(info, 0, sizeof *info);

    img_limits_t lim = resolve_limits(limits);
    if (len == 0) return IMG_EDECODE;
    if (len > lim.max_bytes) return IMG_ETOOBIG;

    img_format_t fmt = sniff(data, len);
    if (fmt == IMG_FMT_UNKNOWN) return IMG_EFORMAT;   /* SVG/HEIC/etc. land here */

    /* Header-only dimension probe — NO full decode (this is the bomb guard). */
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory((const stbi_uc *)data, (int)len, &w, &h, &comp))
        return IMG_EDECODE;
    if (w <= 0 || h <= 0) return IMG_EDECODE;
    if (w > lim.max_width || h > lim.max_height) return IMG_ETOOBIG;
    if ((long)w * (long)h > lim.max_pixels) return IMG_ETOOBIG;

    info->format = fmt;
    info->width  = w;
    info->height = h;
    return IMG_OK;
}

/* ---- growable buffer for the JPEG write callback ------------------------- */
typedef struct { unsigned char *p; size_t len, cap; int err; } growbuf;

static void gb_write(void *ctx, void *data, int size) {
    growbuf *b = ctx;
    if (b->err || size <= 0) return;
    if (b->len + (size_t)size > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 64 * 1024;
        while (ncap < b->len + (size_t)size) ncap *= 2;
        unsigned char *np = realloc(b->p, ncap);
        if (!np) { b->err = 1; return; }
        b->p = np; b->cap = ncap;
    }
    memcpy(b->p + b->len, data, (size_t)size);
    b->len += (size_t)size;
}

/* Fit (w,h) within a `box` longest-edge square, downscale only. */
static void fit_box(int w, int h, int box, int *ow, int *oh) {
    if (box <= 0 || (w <= box && h <= box)) { *ow = w; *oh = h; return; }
    double scale = (double)box / (double)(w >= h ? w : h);
    int nw = (int)(w * scale + 0.5);
    int nh = (int)(h * scale + 0.5);
    *ow = nw < 1 ? 1 : nw;
    *oh = nh < 1 ? 1 : nh;
}

int image_reencode(const void *data, size_t len,
                   const img_limits_t *limits, const img_encode_opts_t *opts,
                   void **out, size_t *out_len) {
    if (!out || !out_len) return IMG_EINVAL;
    *out = NULL; *out_len = 0;

    img_info_t info;
    int rc = image_validate(data, len, limits, &info);
    if (rc != IMG_OK) return rc;

    /* Decode forcing 3-channel RGB (alpha dropped; JPEG output has no alpha). */
    int w = 0, h = 0, comp = 0;
    unsigned char *rgb = stbi_load_from_memory((const stbi_uc *)data, (int)len,
                                               &w, &h, &comp, 3);
    if (!rgb) return IMG_EDECODE;

    int max_dim = opts ? opts->max_dim : 0;
    int q = (opts && opts->jpeg_quality) ? opts->jpeg_quality : DEF_JPEG_Q;
    if (q < 1) q = 1;
    if (q > 100) q = 100;

    int ow, oh;
    fit_box(w, h, max_dim, &ow, &oh);

    unsigned char *pixels = rgb;
    unsigned char *resized = NULL;
    if (ow != w || oh != h) {
        resized = stbir_resize_uint8_srgb(rgb, w, h, 0, NULL, ow, oh, 0, STBIR_RGB);
        if (!resized) { stbi_image_free(rgb); return IMG_ENOMEM; }
        pixels = resized;
    }

    growbuf b = {0};
    int wrote = stbi_write_jpg_to_func(gb_write, &b, ow, oh, 3, pixels, q);

    stbi_image_free(rgb);
    free(resized);

    if (!wrote || b.err) { free(b.p); return b.err ? IMG_ENOMEM : IMG_EENCODE; }

    *out = b.p;
    *out_len = b.len;
    return IMG_OK;
}
