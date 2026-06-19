# Generate a C source that embeds every file under WEBDIR as a byte array, with a
# lookup table (path -> bytes, length, content-type). Run via:
#   cmake -DWEBDIR=<dir> -DOUTFILE=<file> -P embed_assets.cmake
file(GLOB_RECURSE FILES RELATIVE "${WEBDIR}" "${WEBDIR}/*")
list(SORT FILES)

set(DECLS "")
set(ENTRIES "")
set(I 0)
foreach(F ${FILES})
    file(READ "${WEBDIR}/${F}" HEX HEX)
    string(LENGTH "${HEX}" HEXLEN)
    math(EXPR LEN "${HEXLEN} / 2")
    string(REGEX REPLACE "(..)" "0x\\1," BYTES "${HEX}")

    if(F MATCHES "\\.html$")
        set(CT "text/html; charset=utf-8")
    elseif(F MATCHES "\\.js$")
        set(CT "application/javascript; charset=utf-8")
    elseif(F MATCHES "\\.css$")
        set(CT "text/css; charset=utf-8")
    elseif(F MATCHES "\\.svg$")
        set(CT "image/svg+xml")
    elseif(F MATCHES "\\.json$")
        set(CT "application/json")
    elseif(F MATCHES "\\.ico$")
        set(CT "image/x-icon")
    else()
        set(CT "application/octet-stream")
    endif()

    set(DECLS  "${DECLS}static const unsigned char a${I}[] = {${BYTES}};\n")
    set(ENTRIES "${ENTRIES}    { \"/${F}\", a${I}, ${LEN}u, \"${CT}\" },\n")
    if(F STREQUAL "index.html")   # also serve the SPA at "/"
        set(ENTRIES "${ENTRIES}    { \"/\", a${I}, ${LEN}u, \"${CT}\" },\n")
    endif()
    math(EXPR I "${I}+1")
endforeach()

file(WRITE "${OUTFILE}"
"/* GENERATED from web/ by cmake/embed_assets.cmake — do not edit. */\n"
"#include \"handlers/web_assets.h\"\n\n"
"${DECLS}\n"
"const pgf_asset_t PGF_ASSETS[] = {\n${ENTRIES}};\n"
"const int PGF_ASSETS_COUNT = (int)(sizeof(PGF_ASSETS) / sizeof(PGF_ASSETS[0]));\n")
