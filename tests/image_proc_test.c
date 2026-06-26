/* cellar — image_proc unit test (no DB).
 *
 * Fixtures are generated in-memory: valid JPEG/PNG via stb's encoder, and a
 * decompression-bomb PNG hand-built as an 8-byte sig + IHDR declaring huge
 * dimensions (so the area guard fires from the header, before any decode). */
#include "image_proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

static int failures = 0;
static void chk(const char *label, int cond) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", label);
    if (!cond) failures++;
}

/* ---- in-memory encode sink ---------------------------------------------- */
typedef struct { unsigned char *p; size_t len, cap; } sink;
static void sink_write(void *ctx, void *data, int size) {
    sink *s = ctx;
    if (s->len + (size_t)size > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 65536;
        while (nc < s->len + (size_t)size) nc *= 2;
        s->p = realloc(s->p, nc); s->cap = nc;
    }
    memcpy(s->p + s->len, data, (size_t)size); s->len += (size_t)size;
}

/* A w*h RGB gradient raster. */
static unsigned char *make_raster(int w, int h) {
    unsigned char *r = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *px = r + ((size_t)y * w + x) * 3;
            px[0] = (unsigned char)(x * 255 / (w - 1));
            px[1] = (unsigned char)(y * 255 / (h - 1));
            px[2] = (unsigned char)((x + y) & 0xFF);
        }
    return r;
}

static void put_be32(unsigned char *p, unsigned v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;  p[3] = v & 0xFF;
}

/* Minimal PNG: signature + IHDR(w,h, 8-bit RGB) + an empty IDAT chunk header.
 * stb's header scan reads past IHDR and only reports dimensions once it reaches
 * an IDAT/tRNS chunk, so the IDAT marker is required (its data is not — the bomb
 * is rejected from the header, pixel data is never decoded). */
static size_t make_png_header(unsigned w, unsigned h, unsigned char *buf) {
    static const unsigned char sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    memcpy(buf, sig, 8);
    put_be32(buf + 8, 13);                 /* IHDR length */
    memcpy(buf + 12, "IHDR", 4);
    put_be32(buf + 16, w);
    put_be32(buf + 20, h);
    buf[24] = 8;                           /* bit depth   */
    buf[25] = 2;                           /* color type RGB */
    buf[26] = 0; buf[27] = 0; buf[28] = 0; /* comp/filter/interlace */
    put_be32(buf + 29, 0);                 /* CRC (stb does not verify) */
    put_be32(buf + 33, 0);                 /* IDAT length 0 */
    memcpy(buf + 37, "IDAT", 4);           /* IDAT marker => stb reports dims */
    return 41;
}

int main(void) {
    printf("image_proc (stb)\n");

    /* ---- build valid fixtures (400x300) ---- */
    int W = 400, H = 300;
    unsigned char *raster = make_raster(W, H);

    sink png = {0}, jpg = {0};
    stbi_write_png_to_func(sink_write, &png, W, H, 3, raster, W * 3);
    stbi_write_jpg_to_func(sink_write, &jpg, W, H, 3, raster, 90);
    free(raster);

    img_info_t info;

    /* ---- validate: happy paths ---- */
    printf("validate\n");
    chk("png valid", image_validate(png.p, png.len, NULL, &info) == IMG_OK);
    chk("png format", info.format == IMG_FMT_PNG);
    chk("png dims", info.width == W && info.height == H);

    chk("jpg valid", image_validate(jpg.p, jpg.len, NULL, &info) == IMG_OK);
    chk("jpg format", info.format == IMG_FMT_JPEG);
    chk("jpg dims", info.width == W && info.height == H);

    /* ---- validate: rejections ---- */
    chk("NULL => EINVAL",  image_validate(NULL, 10, NULL, &info) == IMG_EINVAL);
    chk("empty => EDECODE", image_validate("", 0, NULL, &info) == IMG_EDECODE);
    chk("svg => EFORMAT",  image_validate("<svg xmlns=\"x\"></svg>", 20, NULL, &info) == IMG_EFORMAT);
    chk("gif => EFORMAT",  image_validate("GIF89a\x10\x00\x10\x00", 10, NULL, &info) == IMG_EFORMAT);
    chk("garbage => EFORMAT", image_validate("not an image at all..", 21, NULL, &info) == IMG_EFORMAT);

    /* truncated PNG header (valid magic, junk after) => EDECODE */
    unsigned char junk[16]; memcpy(junk, png.p, 8); memset(junk + 8, 0xAB, 8);
    chk("truncated png => EDECODE", image_validate(junk, 16, NULL, &info) == IMG_EDECODE);

    /* ---- decompression-bomb guards (header-only, no decode) ----
     * Dimensions are chosen to land where OUR policy caps (50 MP / 20000px) bind
     * but stb's own backstops (STBI_MAX_DIMENSIONS=32768/axis, ~358 MP area) do
     * not — so these assert OUR pre-decode guard, not stb's. */
    printf("bomb guards\n");
    unsigned char bomb[64];

    size_t bn = make_png_header(10000, 10000, bomb);   /* 100 MP, each axis < 20000 */
    chk("area bomb => ETOOBIG", image_validate(bomb, bn, NULL, &info) == IMG_ETOOBIG);

    bn = make_png_header(30000, 100, bomb);            /* width > max_width 20000 */
    chk("width bomb => ETOOBIG", image_validate(bomb, bn, NULL, &info) == IMG_ETOOBIG);

    /* An absurd declaration trips stb's own backstop first — still rejected,
     * still no decode (defense-in-depth; the exact code may be ETOOBIG/EDECODE). */
    bn = make_png_header(200000, 200000, bomb);
    int absurd = image_validate(bomb, bn, NULL, &info);
    chk("absurd bomb rejected", absurd == IMG_ETOOBIG || absurd == IMG_EDECODE);

    /* oversized input bytes via a tight cap */
    img_limits_t tight; image_limits_default(&tight); tight.max_bytes = 10;
    chk("oversized bytes => ETOOBIG", image_validate(png.p, png.len, &tight, &info) == IMG_ETOOBIG);

    /* a modest custom cap still admits the 400x300 fixture */
    img_limits_t okcap; image_limits_default(&okcap); okcap.max_pixels = 500000; /* 0.5 MP */
    chk("120k px under 0.5MP cap ok", image_validate(png.p, png.len, &okcap, &info) == IMG_OK);

    /* ---- re-encode ---- */
    printf("re-encode\n");
    void *out = NULL; size_t outn = 0;

    /* downscale to a 128 box => 128x96, output is canonical JPEG */
    img_encode_opts_t thumb = { .max_dim = 128, .jpeg_quality = 80 };
    chk("reencode png->thumb ok", image_reencode(png.p, png.len, NULL, &thumb, &out, &outn) == IMG_OK);
    chk("thumb has bytes", out && outn > 0);
    chk("thumb is JPEG", outn > 0 && image_validate(out, outn, NULL, &info) == IMG_OK && info.format == IMG_FMT_JPEG);
    chk("thumb fit box", info.width == 128 && info.height == 96);
    free(out); out = NULL;

    /* no resize (max_dim 0) keeps original dims, still re-encoded to JPEG */
    img_encode_opts_t same = { .max_dim = 0, .jpeg_quality = 0 };
    chk("reencode no-resize ok", image_reencode(jpg.p, jpg.len, NULL, &same, &out, &outn) == IMG_OK);
    chk("no-resize keeps dims", image_validate(out, outn, NULL, &info) == IMG_OK &&
                                info.width == W && info.height == H && info.format == IMG_FMT_JPEG);
    free(out); out = NULL;

    /* upscaling is refused (downscale-only): box bigger than source => unchanged */
    img_encode_opts_t big = { .max_dim = 4000, .jpeg_quality = 75 };
    chk("no upscale", image_reencode(png.p, png.len, NULL, &big, &out, &outn) == IMG_OK &&
                      image_validate(out, outn, NULL, &info) == IMG_OK &&
                      info.width == W && info.height == H);
    free(out); out = NULL;

    /* malformed input is rejected, not crashed */
    chk("reencode garbage => err", image_reencode("garbage bytes here!!", 20, NULL, &thumb, &out, &outn) != IMG_OK);
    chk("reencode garbage out NULL", out == NULL);

    /* a header-valid but bodyless PNG fails at decode, cleanly */
    bn = make_png_header(64, 64, bomb);
    chk("reencode headeronly => err", image_reencode(bomb, bn, NULL, &thumb, &out, &outn) != IMG_OK);

    free(png.p); free(jpg.p);

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
