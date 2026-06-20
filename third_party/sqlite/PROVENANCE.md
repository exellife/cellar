# SQLite — vendored amalgamation

- **Version:** 3.45.1 (`SQLITE_VERSION` in `include/sqlite3.h`)
- **Source:** https://sqlite.org/2024/sqlite-amalgamation-3450100.zip
- **Files:** `sqlite3.c` (amalgamation), `include/sqlite3.h`, `include/sqlite3ext.h`
  (the `shell.c` CLI driver is intentionally not vendored).

Compiled directly into the build by the top-level `CMakeLists.txt` (target
`sqlite3`, aliased `SQLite::SQLite3`) with our own compile-time flags — no system
`libsqlite3` dependency. To upgrade, replace these files with a newer
amalgamation and update the version above.
