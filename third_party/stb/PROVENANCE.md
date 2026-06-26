# third_party/stb

Vendored single-header libraries from https://github.com/nothings/stb
(public domain / MIT — see the license block at the foot of each header).

Pinned at commit `31c1ad37456438565541f4919958214b6e762fb4` (master, fetched 2026-06-26):

| Header | Version | Used for |
|---|---|---|
| `stb_image.h` | v2.30 | decode JPEG/PNG (+ header-only dimension probe, `stbi_info_from_memory`) |
| `stb_image_resize2.h` | v2.18 | high-quality resize (`stbir_resize_uint8_srgb`) |
| `stb_image_write.h` | v1.16 | re-encode to JPEG/PNG |

The implementation translation unit lives at `src/core/stb_impl.c` (defines the
`STB_*_IMPLEMENTATION` macros once and includes these headers). Everything else
includes the headers as declarations only.

To bump: drop newer headers in `include/stb/`, update the SHA + versions above,
and re-run the `image_proc` test.
