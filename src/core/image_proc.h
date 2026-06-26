/* ============================================================================
 * cellar — image processing: validate, downscale, re-encode.
 *
 * A pure engine lib (no swappable port — image decode/encode does not have a
 * second backend the way storage does; engine-modules.md marks it "no swap").
 * Backed by vendored stb (third_party/stb), restricted to JPEG + PNG decode.
 *
 * The server never trusts an uploaded image. The mandatory job here is:
 *   1. VALIDATE from the header only — sniff format, read dimensions WITHOUT a
 *      full decode, and reject before committing memory (decompression-bomb
 *      guard). SVG / HEIC / anything not JPEG-or-PNG is rejected here.
 *   2. RE-ENCODE — decode, optionally downscale to a box, drop ALL metadata, and
 *      write a fresh canonical JPEG. This neutralizes embedded exploits/polyglots
 *      and re-derives the source of truth (we never serve user bytes verbatim).
 *
 * Client-side pre-resize (mobile/web) is a complementary optimization; this lib
 * stays mandatory regardless. See engine-modules.md ("Media is mostly modular").
 * ============================================================================ */
#ifndef CEL_IMAGE_PROC_H
#define CEL_IMAGE_PROC_H

#include <stddef.h>

/* Return codes. 0 == success; negative == failure. */
enum {
    IMG_OK      =  0,
    IMG_EINVAL  = -1,   /* bad argument                                      */
    IMG_EFORMAT = -2,   /* unrecognized / unsupported format (not JPEG/PNG)  */
    IMG_ETOOBIG = -3,   /* exceeds a configured cap (bytes / dims / pixels)  */
    IMG_EDECODE = -4,   /* malformed / truncated image data                  */
    IMG_EENCODE = -5,   /* re-encode failed                                  */
    IMG_ENOMEM  = -6    /* allocation failed                                 */
};

typedef enum {
    IMG_FMT_UNKNOWN = 0,
    IMG_FMT_JPEG,
    IMG_FMT_PNG
} img_format_t;

typedef struct {
    img_format_t format;
    int          width;
    int          height;
} img_info_t;

/* Validation caps. A field of 0 means "use the built-in default". */
typedef struct {
    size_t max_bytes;    /* reject input larger than this (input-size guard)  */
    int    max_width;    /* reject declared width  over this (pre-decode)     */
    int    max_height;   /* reject declared height over this (pre-decode)     */
    long   max_pixels;   /* reject width*height over this (area-bomb guard)   */
} img_limits_t;

/* Fill `l` with sane defaults (30 MiB, 20000x20000, 50 MP). */
void image_limits_default(img_limits_t *l);

/* Sniff format + read dimensions from the header ONLY (no full decode), then
 * enforce `limits` (NULL => defaults). Fills `info`. Returns IMG_OK, or
 * IMG_EFORMAT / IMG_ETOOBIG / IMG_EDECODE / IMG_EINVAL. Always run this before
 * touching pixels. */
int image_validate(const void *data, size_t len,
                   const img_limits_t *limits, img_info_t *info);

/* Re-encode options. */
typedef struct {
    int max_dim;       /* fit longest edge within this box; downscale only.
                        * 0 => no resize (still decodes + re-encodes).        */
    int jpeg_quality;  /* 1..100; 0 => default (82).                         */
} img_encode_opts_t;

/* Validate, decode, optionally downscale to fit `opts->max_dim`, strip all
 * metadata, and re-encode to a fresh JPEG. Allocates `*out` (caller frees with
 * free()) and sets `*out_len`. The canonical "re-derive the source of truth"
 * path. Returns IMG_OK or a negative code; on failure `*out` is NULL. */
int image_reencode(const void *data, size_t len,
                   const img_limits_t *limits, const img_encode_opts_t *opts,
                   void **out, size_t *out_len);

#endif /* CEL_IMAGE_PROC_H */
