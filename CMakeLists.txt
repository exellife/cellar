cmake_minimum_required(VERSION 3.16)
project(cellar C)

# ---------------------------------------------------------------------------
# Project-wide settings
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)              # gnu11 (needed by vendored libs)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_definitions(_GNU_SOURCE)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

option(CELLAR_BUILD_TESTS "Enable CTest integration (python harness)" ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
option(CELLAR_STATIC "Statically link bundled deps (libsodium, libuuid)" OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# cJSON is vendored (single-file, MIT) — no libcjson runtime dependency. portico
# vendors the same single-file cJSON and registers its own `cjson` target during
# add_subdirectory() below, so we define ours only if portico hasn't (guarded after
# the portico include) to avoid a duplicate-target collision.

# libsodium / libuuid: dynamic by default, static archives when CELLAR_STATIC.
if(CELLAR_STATIC)
    set(_orig_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
    find_library(SODIUM_LIB sodium REQUIRED)
    find_library(UUID_LIB   uuid   REQUIRED)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_orig_suffixes})
    message(STATUS "cellar: static deps -> ${SODIUM_LIB}, ${UUID_LIB}")
else()
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
    pkg_check_modules(UUID   REQUIRED IMPORTED_TARGET uuid)
    set(SODIUM_LIB PkgConfig::SODIUM)
    set(UUID_LIB   PkgConfig::UUID)
endif()

# ---------------------------------------------------------------------------
# SQLite — the per-app data engine (Phase 1: Postgres → file-per-app SQLite).
# Vendored amalgamation under third_party/sqlite (pinned at 3.45.1), compiled
# straight into the build — no system dependency, so a fresh clone builds
# reproducibly (mirrors third_party/cjson). We own the version and the
# compile-time flags. To bump: drop a newer sqlite3.c/.h in third_party/sqlite.
# ---------------------------------------------------------------------------
add_library(sqlite3 STATIC ${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${CMAKE_SOURCE_DIR}/third_party/sqlite/include)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_DQS=0                  # double-quoted tokens are identifiers only (matches our builder)
    SQLITE_THREADSAFE=1           # serialized default (app_db opens connections NOMUTEX)
    SQLITE_DEFAULT_MEMSTATUS=0    # perf: skip global malloc accounting
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_OMIT_DEPRECATED
    SQLITE_ENABLE_FTS5           # full-text search (generic capability; used by app FTS indexes)
    SQLITE_USE_ALLOCA)
target_link_libraries(sqlite3 PRIVATE ${CMAKE_DL_LIBS} m Threads::Threads)
add_library(SQLite::SQLite3 ALIAS sqlite3)
message(STATUS "cellar: SQLite -> vendored amalgamation third_party/sqlite (3.45.1)")

# ---------------------------------------------------------------------------
# LuaJIT — the Phase-2 hook runtime (design §7-9). Vendored source under
# third_party/luajit (pinned v2.1), built from source OUT of the committed tree
# (copied into <build>/luajit, made there) so the source stays pristine. Yields
# a static libluajit.a — GC64, -fPIC so it links into cellar's PIE executable.
# Hooks reach cellar's C API via FFI, so anything linking it must export its
# dynamic symbols (-rdynamic on the final executable).
# ---------------------------------------------------------------------------
set(LUAJIT_SRC   ${CMAKE_SOURCE_DIR}/third_party/luajit)
set(LUAJIT_BUILD ${CMAKE_BINARY_DIR}/luajit)
set(LUAJIT_LIB   ${LUAJIT_BUILD}/src/libluajit.a)
add_custom_command(
    OUTPUT ${LUAJIT_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${LUAJIT_SRC} ${LUAJIT_BUILD}
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${LUAJIT_BUILD} BUILDMODE=static XCFLAGS=-fPIC amalg
    COMMENT "Building vendored LuaJIT (static, GC64, -fPIC)"
    VERBATIM)
add_custom_target(luajit_build DEPENDS ${LUAJIT_LIB})
add_library(luajit STATIC IMPORTED GLOBAL)
set_target_properties(luajit PROPERTIES
    IMPORTED_LOCATION ${LUAJIT_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${LUAJIT_SRC}/src   # headers are pristine source
    INTERFACE_LINK_LIBRARIES "${CMAKE_DL_LIBS};m")
add_dependencies(luajit luajit_build)
message(STATUS "cellar: LuaJIT -> vendored source third_party/luajit (built static, GC64)")

# ---------------------------------------------------------------------------
# portico — the HTTP/1.1 + WebSocket transport library (sibling project).
# Provides the WebSocket core (replaces the old vendored lib/wslib) plus the
# HTTP layer used by the REST front door. Override PORTICO_DIR to relocate.
# ---------------------------------------------------------------------------
# Prefer a vendored submodule (external/portico) if present, else the sibling
# checkout (../portico) for local co-development. Override with -DPORTICO_DIR=...
if(NOT DEFINED PORTICO_DIR)
    if(EXISTS "${CMAKE_SOURCE_DIR}/external/portico/CMakeLists.txt")
        set(PORTICO_DIR "${CMAKE_SOURCE_DIR}/external/portico")
    else()
        set(PORTICO_DIR "${CMAKE_SOURCE_DIR}/../portico")
    endif()
endif()
if(NOT EXISTS "${PORTICO_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "portico not found at ${PORTICO_DIR} "
            "(set -DPORTICO_DIR=... or add the submodule at external/portico)")
endif()
set(PORTICO_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(PORTICO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${PORTICO_DIR} ${CMAKE_BINARY_DIR}/portico-build)

# cJSON: reuse portico's target if it registered one, else build our own copy.
if(NOT TARGET cjson)
    add_library(cjson STATIC third_party/cjson/cJSON.c)
    target_include_directories(cjson PUBLIC third_party/cjson/include)
    target_compile_options(cjson PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# Remaining vendored libraries (kept warning-quiet).
# ---------------------------------------------------------------------------
add_library(opcode_dispatcher STATIC lib/opcode_dispatcher/src/opcode_dispatcher.c)
target_include_directories(opcode_dispatcher PUBLIC lib/opcode_dispatcher/include)
target_compile_options(opcode_dispatcher PRIVATE -w)
target_link_libraries(opcode_dispatcher PUBLIC Threads::Threads)

add_library(logger STATIC lib/logger/src/logger.c)
target_include_directories(logger PUBLIC lib/logger/include)
target_compile_options(logger PRIVATE -w)

# ---------------------------------------------------------------------------
# cellar executable
# ---------------------------------------------------------------------------
# Embed the web/ admin UI into the binary (generated C byte arrays).
set(WEB_DIR ${CMAKE_SOURCE_DIR}/web)
file(GLOB_RECURSE WEB_FILES CONFIGURE_DEPENDS ${WEB_DIR}/*)
set(WEB_ASSETS_C ${CMAKE_BINARY_DIR}/generated/web_assets.c)
add_custom_command(
    OUTPUT ${WEB_ASSETS_C}
    COMMAND ${CMAKE_COMMAND} -DWEBDIR=${WEB_DIR} -DOUTFILE=${WEB_ASSETS_C}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
    DEPENDS ${WEB_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
    COMMENT "Embedding web/ admin UI assets"
    VERBATIM)

# Note: the old SQL-migration embedding is gone — each app's SQLite bundle owns
# its schema, and cellar's own identity tables are applied per-app at runtime by
# auth_schema.c (version-stamped via PRAGMA user_version).

file(GLOB_RECURSE CELLAR_SOURCES CONFIGURE_DEPENDS src/*.c)
# Vendored stb single-header impl (third_party/stb): compile the generated code
# with warnings off, like cjson/sqlite. Our own image_proc.c keeps full warnings.
set_source_files_properties(src/core/stb_impl.c PROPERTIES COMPILE_OPTIONS "-w")

add_executable(cellar ${CELLAR_SOURCES} ${WEB_ASSETS_C})
target_include_directories(cellar PRIVATE src src/core third_party/stb/include)
# OpenSSL: HMAC-SHA1 for TOTP (RFC 6238) + RS256 verification for OIDC ID tokens.
find_package(OpenSSL REQUIRED)
# libcurl: fetch OIDC providers' JWKS (public keys) over HTTPS for OAuth sign-in.
find_package(CURL REQUIRED)

target_link_libraries(cellar PRIVATE
    portico
    opcode_dispatcher
    logger
    cjson
    SQLite::SQLite3
    luajit
    OpenSSL::Crypto
    CURL::libcurl
    ${SODIUM_LIB}
    ${UUID_LIB}
    m
    Threads::Threads)
# Hooks call cellar's C API via FFI → export the binary's dynamic symbols so
# LuaJIT's ffi.C can resolve them. Build LuaJIT before cellar links.
target_link_options(cellar PRIVATE -rdynamic)
add_dependencies(cellar luajit_build)

# ---------------------------------------------------------------------------
# Install (cmake --install build-cmake [--prefix /usr/local])
#   - the binary (everything else is embedded)
#   - the systemd unit + env-file template under share/cellar
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS cellar RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES dist/cellar.service dist/cellar.env.example
        DESTINATION ${CMAKE_INSTALL_DATADIR}/cellar)

# ---------------------------------------------------------------------------
# Tests (CTest). The harness boots its own server, so the only requirement is
# python3 + the `websockets` module and a reachable Postgres.
# ---------------------------------------------------------------------------
if(CELLAR_BUILD_TESTS)
    enable_testing()

    # Fast, DB-free unit test: pins the exact SQL the query builder emits for the
    # single-tenant (Model A) cases — the regression guard for the Phase 7
    # scope-rule generalization. Links query_builder.c alone (catalog stubbed).
    add_executable(query_builder_test tests/query_builder_test.c src/engine/query_builder.c)
    target_include_directories(query_builder_test PRIVATE src/engine)
    target_link_libraries(query_builder_test cjson)
    add_test(NAME query_builder COMMAND query_builder_test)

    # Phase-2 foundation: the LuaJIT hook VM lifecycle (open/run/close), pcall
    # error isolation (compile + runtime faults don't crash), and FFI available
    # in the embedded state. No server/DB needed. Linked -rdynamic isn't required
    # here (the FFI call targets libc, not cellar's own symbols).
    add_executable(cel_lua_test tests/cel_lua_test.c src/engine/cel_lua.c)
    target_include_directories(cel_lua_test PRIVATE src/engine)
    target_link_libraries(cel_lua_test luajit Threads::Threads)
    add_dependencies(cel_lua_test luajit_build)
    add_test(NAME cel_lua COMMAND cel_lua_test)

    # Phase-2 hook value ABI (design §8): the opaque-handle accessors/mutators the
    # Lua/FFI hook surface is built on — total reads, navigation, in-place object
    # mutation. cJSON builds fixtures; the code under test sees only cel_val_t*.
    add_executable(cel_val_test tests/cel_val_test.c src/engine/cel_val.c)
    target_include_directories(cel_val_test PRIVATE src/engine)
    target_link_libraries(cel_val_test cjson)
    add_test(NAME cel_val COMMAND cel_val_test)

    # Sync Slice 0 (T2): the per-app rev source — _sync_seq seeding, monotonic
    # next_rev, idempotent ensure, and rev-rolls-back-with-the-txn.
    add_executable(cel_sync_test tests/cel_sync_test.c src/engine/cel_sync.c)
    target_include_directories(cel_sync_test PRIVATE src/engine lib/logger/include)
    target_link_libraries(cel_sync_test logger SQLite::SQLite3 Threads::Threads)
    add_test(NAME cel_sync COMMAND cel_sync_test)

    # Phase-2 hook dispatcher (design §8): the `cellar` prelude boxes opaque
    # cel_val_t* payloads into Lua proxies, authorize/before run with table-like
    # sugar, before() mutates input in place, absent hooks no-op, faults fail
    # closed. Linked -rdynamic so the prelude's ffi.C resolves cellar's symbols.
    # cel_hooks.c grew hard deps on apps/realtime/policy/jobq (rt_emit + run_jobs),
    # so the unit target links the same engine slice as cel_apps_test.
    add_executable(cel_hooks_test tests/cel_hooks_test.c
        src/engine/cel_hooks.c src/engine/cel_lua.c src/engine/cel_val.c
        src/engine/cel_apps.c src/core/app_db.c src/core/auth_schema.c
        src/engine/schema_catalog.c src/engine/schema_catalog_sqlite.c src/engine/policy.c
        src/engine/cel_control.c src/engine/cel_sync.c src/engine/realtime.c src/core/metrics.c
        src/core/event_sink_sqlite.c src/core/job_queue_sqlite.c src/engine/cel_hook_state.c)
    target_include_directories(cel_hooks_test PRIVATE src src/engine src/core lib/logger/include)
    target_link_libraries(cel_hooks_test luajit cjson logger SQLite::SQLite3 Threads::Threads)
    target_link_options(cel_hooks_test PRIVATE -rdynamic)
    add_dependencies(cel_hooks_test luajit_build)
    add_test(NAME cel_hooks COMMAND cel_hooks_test)

    # Phase-2 per-app hook VM cache (design §9): lazy per-(thread×app) state, load
    # from <bundle>/hooks.lua, mtime hot-reload, compile-error recovery, per-thread
    # independence. No server needed (drives a temp bundle dir).
    add_executable(cel_hook_state_test tests/cel_hook_state_test.c
        src/engine/cel_hook_state.c src/engine/cel_hooks.c src/engine/cel_lua.c src/engine/cel_val.c
        src/engine/cel_apps.c src/core/app_db.c src/core/auth_schema.c
        src/engine/schema_catalog.c src/engine/schema_catalog_sqlite.c src/engine/policy.c
        src/engine/cel_control.c src/engine/cel_sync.c src/engine/realtime.c src/core/metrics.c
        src/core/event_sink_sqlite.c src/core/job_queue_sqlite.c)
    target_include_directories(cel_hook_state_test PRIVATE src src/engine src/core lib/logger/include)
    target_link_libraries(cel_hook_state_test luajit cjson logger SQLite::SQLite3 Threads::Threads)
    target_link_options(cel_hook_state_test PRIVATE -rdynamic)
    add_dependencies(cel_hook_state_test luajit_build)
    add_test(NAME cel_hook_state COMMAND cel_hook_state_test)

    # DB-free unit test for the per-app SQLite handle layer (Phase 1 foundation):
    # open-by-file, the connection pool, per-app write serialization, concurrent
    # readers, the get-by-path cache, and isolation between app files. Creates
    # throwaway SQLite files under /tmp; needs no Postgres.
    add_executable(app_db_test tests/app_db_test.c src/core/app_db.c)
    target_include_directories(app_db_test PRIVATE src/core)
    target_link_libraries(app_db_test SQLite::SQLite3 logger Threads::Threads)
    add_test(NAME app_db COMMAND app_db_test)

    # Regression for C-1/H-1: a pinned app_db handle must survive LRU eviction
    # (else the app registry's cached pointer dangles → remote use-after-free).
    # Built with a tiny CEL_APP_MAX_OPEN so eviction triggers in-test.
    add_executable(app_db_evict_test tests/app_db_evict_test.c src/core/app_db.c)
    target_include_directories(app_db_evict_test PRIVATE src/core)
    target_compile_definitions(app_db_evict_test PRIVATE CEL_APP_MAX_OPEN=4)
    target_link_libraries(app_db_evict_test SQLite::SQLite3 logger Threads::Threads)
    add_test(NAME app_db_evict COMMAND app_db_evict_test)

    # DB-free unit test for the SQLite result -> typed JSON serializer (Phase 1):
    # catalog-typed cells (int/real/bool/json/null/text) and the runtime-typed
    # table-less path. Links result_json.c against an in-memory SQLite db; the
    # test supplies its own cel_table_column, so it pulls in neither
    # schema_catalog.c nor Postgres.
    add_executable(result_json_test tests/result_json_test.c src/engine/result_json.c)
    target_include_directories(result_json_test PRIVATE src/engine)
    target_link_libraries(result_json_test SQLite::SQLite3 cjson)
    add_test(NAME result_json COMMAND result_json_test)

    # DB-free unit test for the per-app SQLite schema catalog (Phase 1 step 3):
    # introspection via sqlite_master + pragma_table_info / foreign_key_list,
    # declared-type affinity mapping, PK/FK/nullable/default flags, and exclusion
    # of cel_* / sqlite_* internal tables. In-memory SQLite; no Postgres.
    add_executable(schema_catalog_sqlite_test
        tests/schema_catalog_sqlite_test.c src/engine/schema_catalog_sqlite.c)
    target_include_directories(schema_catalog_sqlite_test PRIVATE src/engine)
    target_link_libraries(schema_catalog_sqlite_test SQLite::SQLite3 logger)
    add_test(NAME schema_catalog_sqlite COMMAND schema_catalog_sqlite_test)

    # End-to-end (Phase 1 steps 2-4): the query builder's SQLite dialect (?N +
    # RETURNING) actually executes — build -> bind -> step -> serialize, with the
    # catalog from real introspection. Prototypes the step-5 executor. No Postgres.
    add_executable(query_exec_sqlite_test tests/query_exec_sqlite_test.c
        src/engine/query_builder.c src/engine/result_json.c src/engine/schema_catalog_sqlite.c)
    target_include_directories(query_exec_sqlite_test PRIVATE src/engine)
    target_link_libraries(query_exec_sqlite_test SQLite::SQLite3 cjson logger)
    add_test(NAME query_exec_sqlite COMMAND query_exec_sqlite_test)

    # The identity layer on per-app SQLite (Phase 1 step 5b): register / login /
    # verify / logout / password-reset / create-user / seed against a throwaway
    # SQLite app. MFA stubbed off in the test, so no Postgres. libpq-free.
    add_executable(auth_sqlite_test tests/auth_sqlite_test.c
        src/core/auth.c src/core/auth_schema.c src/core/app_db.c src/core/db_sqlite.c
        src/core/password.c src/core/session_cache.c src/core/metrics.c)
    target_include_directories(auth_sqlite_test PRIVATE src/core src)
    target_link_libraries(auth_sqlite_test SQLite::SQLite3 logger Threads::Threads
        ${SODIUM_LIB} ${UUID_LIB})
    add_test(NAME auth_sqlite COMMAND auth_sqlite_test)

    # App registry + Host routing (Phase 1): single-app + multi-app resolution,
    # :port stripping, invalid-host rejection, per-app catalogs, enter/leave
    # thread-local binding. Builds throwaway bundle dirs under /tmp; no Postgres.
    add_executable(cel_apps_test tests/cel_apps_test.c
        src/engine/cel_apps.c src/core/app_db.c src/core/auth_schema.c
        src/engine/schema_catalog.c src/engine/schema_catalog_sqlite.c src/engine/policy.c
        src/engine/cel_control.c src/engine/cel_sync.c src/engine/realtime.c src/core/metrics.c
        src/core/event_sink_sqlite.c src/core/job_queue_sqlite.c
        src/engine/cel_hook_state.c src/engine/cel_hooks.c src/engine/cel_lua.c src/engine/cel_val.c)
    target_include_directories(cel_apps_test PRIVATE src src/engine src/core lib/logger/include)
    target_link_libraries(cel_apps_test SQLite::SQLite3 cjson logger luajit Threads::Threads)
    target_link_options(cel_apps_test PRIVATE -rdynamic)
    add_dependencies(cel_apps_test luajit_build)
    add_test(NAME cel_apps COMMAND cel_apps_test)

    # MFA (TOTP) on per-app SQLite (Phase 1 step 5b): enroll -> confirm (real TOTP
    # code) -> required_for -> challenge -> verify_login (TOTP + recovery code) ->
    # disable -> reset, end-to-end against a throwaway app. Drives the real auth
    # session issuance. No Postgres.
    add_executable(mfa_sqlite_test tests/mfa_sqlite_test.c
        src/core/mfa.c src/core/auth.c src/core/auth_schema.c src/core/app_db.c
        src/core/db_sqlite.c src/core/password.c src/core/session_cache.c
        src/core/metrics.c src/core/totp.c)
    target_include_directories(mfa_sqlite_test PRIVATE src/core src)
    target_link_libraries(mfa_sqlite_test SQLite::SQLite3 logger Threads::Threads
        ${SODIUM_LIB} ${UUID_LIB} OpenSSL::Crypto)
    add_test(NAME mfa_sqlite COMMAND mfa_sqlite_test)

    # DB-free unit test for the declarative-roles authorization engine (#50):
    # built-in defaults stay byte-for-byte, and a "_roles" config bundle defines
    # a custom role vocabulary. Links policy.c (auth stubbed in the test).
    add_executable(policy_test tests/policy_test.c src/engine/policy.c)
    target_include_directories(policy_test PRIVATE src/engine src)
    target_link_libraries(policy_test cjson logger)
    add_test(NAME policy COMMAND policy_test)

    # DB-free unit test for the realtime registry + authz fan-out (#53): predicate
    # matching and subscribe/replace/unsubscribe/drop_conn with a mock transport.
    add_executable(realtime_test tests/realtime_test.c src/engine/realtime.c src/core/metrics.c)
    target_include_directories(realtime_test PRIVATE src/engine src src/core)
    target_link_libraries(realtime_test cjson pthread)
    add_test(NAME realtime COMMAND realtime_test)

    # DB-free unit test for the session cache (#55): hit/miss/refresh/evict, the
    # disabled mode, and TTL expiry. Links session_cache.c alone.
    add_executable(session_cache_test tests/session_cache_test.c src/core/session_cache.c src/core/metrics.c)
    target_include_directories(session_cache_test PRIVATE src/core)
    target_link_libraries(session_cache_test pthread)
    add_test(NAME session_cache COMMAND session_cache_test)

    # DB-free unit test for the token-bucket rate limiter (#Phase 9): burst,
    # deny, per-key isolation, disabled mode, time-based refill.
    add_executable(rate_limit_test tests/rate_limit_test.c src/core/rate_limit.c)
    target_include_directories(rate_limit_test PRIVATE src/core)
    target_link_libraries(rate_limit_test pthread)
    add_test(NAME rate_limit COMMAND rate_limit_test)

    # DB-free unit test for the metrics registry (#Phase 9 observability): counter
    # bumps, gauge provider, and Prometheus text rendering incl. the histogram.
    add_executable(metrics_test tests/metrics_test.c src/core/metrics.c)
    target_include_directories(metrics_test PRIVATE src/core)
    target_link_libraries(metrics_test pthread)
    add_test(NAME metrics COMMAND metrics_test)

    # DB-free TOTP unit test (#auth 2FA): RFC 6238 vectors + generate/verify.
    add_executable(totp_test tests/totp_test.c src/core/totp.c)
    target_include_directories(totp_test PRIVATE src/core)
    target_link_libraries(totp_test OpenSSL::Crypto ${SODIUM_LIB})
    add_test(NAME totp COMMAND totp_test)

    # DB-free CORS origin-policy unit test.
    add_executable(cors_test tests/cors_test.c src/core/cors.c)
    target_include_directories(cors_test PRIVATE src/core)
    add_test(NAME cors COMMAND cors_test)

    # DB-free base64url unit test (backs the keyset cursor tokens).
    add_executable(base64url_test tests/base64url_test.c src/core/base64url.c)
    target_include_directories(base64url_test PRIVATE src/core)
    add_test(NAME base64url COMMAND base64url_test)

    # app-schema migration runner: apply/idempotent/incremental, drift detection,
    # transactional rollback of a failing migration. In-memory DB + /tmp dirs.
    add_executable(migrate_test tests/migrate_test.c src/core/migrate.c)
    target_include_directories(migrate_test PRIVATE src/core)
    target_link_libraries(migrate_test SQLite::SQLite3 Threads::Threads)
    add_test(NAME migrate COMMAND migrate_test)

    # DB-free BlobStore disk-adapter unit test (port-first object store; E0.1).
    add_executable(blob_store_test tests/blob_store_test.c src/core/blob_store_disk.c)
    target_include_directories(blob_store_test PRIVATE src/core)
    target_link_libraries(blob_store_test Threads::Threads)
    add_test(NAME blob_store COMMAND blob_store_test)

    # DB-free image-processing unit test (stb-backed validate/resize/re-encode; E0.2).
    add_executable(image_proc_test tests/image_proc_test.c
        src/core/image_proc.c src/core/stb_impl.c)
    target_include_directories(image_proc_test PRIVATE src/core third_party/stb/include)
    target_link_libraries(image_proc_test m)
    add_test(NAME image_proc COMMAND image_proc_test)

    # EventSink SQLite-adapter unit test (port-first event collection; E2.2).
    add_executable(event_sink_test tests/event_sink_test.c src/core/event_sink_sqlite.c)
    target_include_directories(event_sink_test PRIVATE src/core)
    target_link_libraries(event_sink_test SQLite::SQLite3 Threads::Threads)
    add_test(NAME event_sink COMMAND event_sink_test)

    # JobQueue SQLite-adapter unit test (claim/retry/visibility/recurring; E2.1).
    add_executable(job_queue_test tests/job_queue_test.c src/core/job_queue_sqlite.c)
    target_include_directories(job_queue_test PRIVATE src/core)
    target_link_libraries(job_queue_test SQLite::SQLite3 Threads::Threads)
    add_test(NAME job_queue COMMAND job_queue_test)

    find_program(PYTHON3 python3)
    if(PYTHON3)
        add_test(NAME smoke
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/scripts/smoke_test.py)
        # Media upload/serve (E1.1/E1.2): image lib + BlobStore over the request
        # loop — upload re-encodes to JPEG variants, serve streams them back.
        add_test(NAME media
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/media_test.py)
        # classifieds Phase-0 bundle: validation (A0.4) + facet-sync (A0.5) hooks
        # on the real write path (self-contained: builds an app db from the
        # bundle schema, boots cellar, drives the REST API + rpc).
        add_test(NAME classifieds_phase0
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/apps/classifieds/test_phase0.py
                    $<TARGET_FILE:cellar>)
        # classifieds chat realtime (A1.6): WS subscribe (VIA membership) +
        # live message delivery + non-participant subscribe denied.
        add_test(NAME classifieds_chat_ws
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/apps/classifieds/test_chat_ws.py
                    $<TARGET_FILE:cellar>)
        # classifieds background worker (E2.1): jobs run autonomously on a timer
        # (the expire sweep flips a listing without any /jobs/run trigger).
        add_test(NAME classifieds_worker
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/apps/classifieds/test_worker.py
                    $<TARGET_FILE:cellar>)
        # classifieds progressive-trust velocity limits (A2.5): a new account's
        # post cap is enforced; admins are exempt.
        add_test(NAME classifieds_velocity
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/apps/classifieds/test_velocity.py
                    $<TARGET_FILE:cellar>)
        # Mailer (no DB): send via the send-test-mail CLI to a local mock SMTP sink
        # and assert the captured message is well-formed RFC 5322.
        add_test(NAME mailer
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/mailer_test.sh $<TARGET_FILE:cellar>)
        # Bundle provisioning (Phase 2 §4): `cellar provision <host>` scaffolds a
        # working app bundle (data.db + hooks.lua + seeded admin) that then serves.
        add_test(NAME provision
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/provision_test.sh $<TARGET_FILE:cellar>)
        # Per-bundle static serving (Phase 2 §4): an app's public/ front-end is
        # served (index, assets, SPA fallback) without shadowing the API routes.
        add_test(NAME static_serve
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/static_serve_test.sh $<TARGET_FILE:cellar>)
        # Bundle export/import (Phase 2 §4): online VACUUM-INTO snapshot + tar of
        # data.db/hooks.lua/public, import under a new host, sessions stripped.
        add_test(NAME export_import
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/export_import_test.sh $<TARGET_FILE:cellar>)
        # Per-app policy (Phase 2 §4): two apps differing only in policies.json get
        # different authz for the same request (policy bound per request).
        add_test(NAME perapp_policy
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/perapp_policy_test.sh $<TARGET_FILE:cellar>)
        # Control-plane registry (design §11): CEL_CONTROL_DB gates routing to
        # registered+active hosts; suspend/resume take effect on a running server.
        add_test(NAME control_plane
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/control_plane_test.sh $<TARGET_FILE:cellar>)
        # Security regression (Phase-2 audit): the bundle CLIs reject symlinked
        # bundle paths (no read/write outside a bundle via provision/export/import).
        add_test(NAME cli_symlink
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/cli_symlink_test.sh $<TARGET_FILE:cellar>)
        # --- Auth/transport e2e (Postgres-era; being ported to the SQLite harness
        #     under Task 8 — see tests/run_with_server.py). RPC is deferred to the
        #     Phase 2 hook layer, so rpc/rpc_tenant/rpc_secdef were removed. ---
        # Auth hardening: tokens hashed at rest, revoke-sessions CLI, password cap.
        add_test(NAME auth_hardening
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/auth_hardening_test.sh $<TARGET_FILE:cellar>)
        # Identity split: credentials live in cel_identities (not cel_users);
        # login resolves through the password identity; created users get one.
        add_test(NAME identity
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/identity_test.sh $<TARGET_FILE:cellar>)
        # TOTP 2FA: enroll -> confirm -> two-step login (challenge + code) ->
        # disable, with codes computed by an independent Python TOTP.
        add_test(NAME mfa
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/mfa_test.sh $<TARGET_FILE:cellar>)
        # OIDC sign-in: verify RS256 ID tokens (mock provider + local JWKS) and
        # find-or-link (auto-provision / replay / link-by-email) + rejections.
        add_test(NAME oauth
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/oauth_test.sh $<TARGET_FILE:cellar>)
        # Password reset: forgot emails a token (captured from a mock SMTP sink),
        # redeem it, new password works / old one + session revoked, single-use.
        add_test(NAME password_reset
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/password_reset_test.sh $<TARGET_FILE:cellar>)
        # Email verification: register emails a token (captured), redeem it, and
        # email_verified flips false -> true; single-use; resend is idempotent.
        add_test(NAME email_verification
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/email_verification_test.sh $<TARGET_FILE:cellar>)
        # Account lockout: N failures lock the account (even a correct password is
        # then refused), the lock auto-expires, and `cellar unlock` clears it.
        add_test(NAME lockout
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/lockout_test.sh $<TARGET_FILE:cellar>)
        # CORS: allowed origin echoed, others denied, OPTIONS preflight -> 204
        # with the CORS headers.
        add_test(NAME cors_e2e
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/cors_test.sh $<TARGET_FILE:cellar>)
        # Input/DoS limits: oversized body -> 413 before parse; a slowloris
        # (incomplete request held open) is reaped after the header timeout.
        add_test(NAME limits
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/limits_test.sh $<TARGET_FILE:cellar>)
        add_test(NAME wslib_adversarial
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/wslib_test.py)
        add_test(NAME rest
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/rest_test.py)
        # OpenAPI: GET /openapi.json is a valid OpenAPI 3.0 doc built from the catalog.
        add_test(NAME openapi
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/openapi_test.py)
        # RPC as a Lua hook (Phase 2 §8): POST /rpc/<fn> dispatches to the bundle's
        # hooks.lua rpc(name,args,who) — scalar/db results, who, args, faults.
        add_test(NAME rpc_hook
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/rpc_test.py)
        # Offline-first sync substrate (Slice 0, T1-T5): the syncable `items` table —
        # monotonic engine-owned rev, force-owned rev/deleted, soft-delete + tombstone
        # invisibility, non-syncable products unaffected, _sync_seq not REST-exposed.
        add_test(NAME sync_rev
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/sync_rev_test.py)
        # Regression: a soft-delete on a syncable+realtime table still pushes a
        # DELETE event (the deleted=0 read-filter must not gate realtime delivery).
        add_test(NAME sync_realtime
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/sync_realtime_test.py)
        # Sync Slice 1: POST /sync/pull delta read — tombstones included, safe
        # {cursor, more}, incremental + paginated pulls, tables filter, 401.
        add_test(NAME sync_pull
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/sync_pull_test.py)
        # Sync Slice 2: POST /sync/push batch apply — client-id creates, clean +
        # conflicting writes (LWW + resolve() override), idempotent del, all-or-
        # nothing rollback, round-trip. Boots with a resolve() bundle.
        add_test(NAME sync_push
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/sync_push_test.sh $<TARGET_FILE:cellar>)
        # Sync Slice 3: idempotent-retry dedup (mutation_id) incl the audit lost-update
        # case, and per-device pull cursors (monotonic, no rewind).
        add_test(NAME sync_dedup
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/sync_dedup_test.py)
        # Sync Slice 3: tombstone GC (cellar sync-gc) honors per-device cursors —
        # purge what every device pulled past, keep the rest. Boots its own server.
        add_test(NAME sync_gc
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/sync_gc_test.sh $<TARGET_FILE:cellar>)
        # Sync cascade soft-delete: deleting a parent on a syncable table cascades the
        # soft-delete (+rev-stamp, so it propagates) to ON DELETE CASCADE children,
        # recursively, leaving non-CASCADE children alone. Boots its own server.
        add_test(NAME sync_cascade
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/sync_cascade_test.sh $<TARGET_FILE:cellar>)
        # CRUD hooks (Phase 2 §8): before/authorize/after on the write path, via a
        # before/authorize/after bundle (CEL_HOOKS_FILE=tests/hooks_crud.lua).
        add_test(NAME hooks_crud
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/hooks_crud_test.sh $<TARGET_FILE:cellar>)
        # on_realtime hook (Phase 2 §8): a delivery filter drops SILENT-titled notes
        # while normal notes still push (CEL_HOOKS_FILE=tests/hooks_realtime.lua).
        add_test(NAME realtime_filter
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/realtime_filter_test.sh $<TARGET_FILE:cellar>)
        # WS keepalive: portico pings idle sockets + reaps silent peers, while
        # PONGing peers survive (CEL_WS_PING_INTERVAL=1 / CEL_WS_PONG_TIMEOUT=2).
        add_test(NAME ws_keepalive
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ws_keepalive_test.sh $<TARGET_FILE:cellar>)
        # Self-service registration (#51): boots with the taxi example policy
        # (rider/driver self-registerable, admin not) and exercises /auth/register.
        add_test(NAME register
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/register_test.sh $<TARGET_FILE:cellar>)
        # Register enumeration (H-5): the secure default returns a uniform 202 for
        # new and already-registered emails alike (no token, no 409 oracle).
        add_test(NAME register_enum
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/register_enum_test.sh $<TARGET_FILE:cellar>)
        # Realtime (#53) end-to-end: WS subscribe -> write -> pushed CHANGE, with the
        # owner-scoped subscription proving the authz filter (needs notes realtime-on).
        add_test(NAME realtime_e2e
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/realtime_e2e.py)
        # Auth rate limit end-to-end: a low per-IP limit makes /auth/login 429.
        add_test(NAME rate_limit_e2e
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/rate_limit_test.sh $<TARGET_FILE:cellar>)
        # Observability (#Phase 9) end-to-end: GET /metrics is a live Prometheus
        # scrape and counters (2xx, latency histogram, failed logins) move with traffic.
        # Bearer-gated (L-3): the wrapper sets CEL_METRICS_TOKEN.
        add_test(NAME metrics_e2e
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/metrics_e2e_test.sh $<TARGET_FILE:cellar>)
        # Richer read API: relationship embedding (to-one + to-many) + exact count.
        add_test(NAME query
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/query_test.py)
        # Keyset/cursor pagination: pages tile the ordered set exactly (no gaps/dups),
        # last page has no cursor, bad cursor -> 400.
        add_test(NAME query_keyset
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/query_keyset_test.py)
        # Boolean filter tree: and/or/not/between via ?where=<json>, live Postgres.
        add_test(NAME query_filter
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/query_filter_test.py)
        # Nested/dotted embed: embed=products.categories nests two levels; depth-capped.
        add_test(NAME query_nested
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/query_nested_test.py)
        # Group-by aggregates: count/sum/avg/min/max grouped by a column, live Postgres.
        add_test(NAME query_agg
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/query_agg_test.py)
        # Embedding authz: an embedded relation enforces the caller's read policy on
        # the related table (categories admin-only -> viewer embed 403, admin 200).
        add_test(NAME query_authz
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/query_authz_test.sh $<TARGET_FILE:cellar>)
        # Headless admin-UI test (jsdom) — only if node + jsdom are present.
        find_program(NODE node)
        if(NODE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/ui/node_modules/jsdom)
            add_test(NAME admin_ui
                COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                        $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/tests/ui/ui_test.mjs)
        endif()
    endif()
endif()
