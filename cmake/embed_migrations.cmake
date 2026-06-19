# Generate a C source embedding every *.sql file under MIGDIR as a NUL-terminated
# string, sorted by filename, into an array named ${ARRAY} (count ${ARRAY}_COUNT).
#   cmake -DMIGDIR=<dir> -DOUTFILE=<file> -DARRAY=CEL_MIGRATIONS -P embed_migrations.cmake
if(NOT DEFINED ARRAY)
    set(ARRAY CEL_MIGRATIONS)
endif()

file(GLOB FILES RELATIVE "${MIGDIR}" "${MIGDIR}/*.sql")
list(SORT FILES)

set(DECLS "")
set(ENTRIES "")
set(I 0)
foreach(F ${FILES})
    file(READ "${MIGDIR}/${F}" HEX HEX)
    string(REGEX REPLACE "(..)" "0x\\1," BYTES "${HEX}")
    set(DECLS   "${DECLS}static const char ${ARRAY}_m${I}[] = {${BYTES}0x00};\n")
    set(ENTRIES "${ENTRIES}    { \"${F}\", ${ARRAY}_m${I} },\n")
    math(EXPR I "${I}+1")
endforeach()

file(WRITE "${OUTFILE}"
"/* GENERATED from ${MIGDIR} by cmake/embed_migrations.cmake — do not edit. */\n"
"#include \"core/migrate.h\"\n\n"
"${DECLS}\n"
"const cel_migration_t ${ARRAY}[] = {\n${ENTRIES}};\n"
"const int ${ARRAY}_COUNT = (int)(sizeof(${ARRAY}) / sizeof(${ARRAY}[0]));\n")
