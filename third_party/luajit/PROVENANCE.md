# LuaJIT — vendored source

- **Version:** 2.1 ROLLING — commit `8e6520a7aecd0517e792b359afbbfd7274791f5f` (2026-06-16)
- **Source:** https://github.com/LuaJIT/LuaJIT (branch `v2.1`)
- **Vendored as a copied source tree** (matches `third_party/cjson` and
  `third_party/sqlite` — the repo is submodule-free, so a fresh clone builds with
  no `--recursive` step).

Built **from source, out of the committed tree** by the top-level `CMakeLists.txt`
(`third_party/luajit` is copied into `<build>/luajit` and `make`d there) into a
static `libluajit.a` — GC64, `-fPIC` so it links into cellar's PIE executable.
Hooks call cellar's C API via FFI, so the final binary is linked `-rdynamic`.

To upgrade: re-copy a newer `v2.1` checkout over this tree (keep it pristine — no
build artifacts) and update the commit hash above.
