/* ============================================================================
 * cellar — stb single-header implementation translation unit.
 *
 * The ONE place the stb *_IMPLEMENTATION macros are defined. Every other file
 * includes the stb headers as declarations only. Kept separate so the (large,
 * warning-noisy) generated code compiles once with relaxed flags.
 *
 * Hardening: memory-only (no stdio file paths), decoder restricted to the two
 * formats we accept (JPEG, PNG) so unused parsers aren't even linked, and a hard
 * STBI_MAX_DIMENSIONS backstop below stb's default — a second line of defense
 * under our own pre-decode dimension cap (image_proc.c).
 * ============================================================================ */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_MAX_DIMENSIONS 32768   /* hard backstop; our policy cap is lower */
#include "stb/stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb/stb_image_write.h"
