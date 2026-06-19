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
option(CELLAR_STATIC "Statically link bundled deps (libsodium, libuuid); libpq stays dynamic" OFF)

find_package(Threads REQUIRED)
find_package(PostgreSQL REQUIRED)       # provides PostgreSQL::PostgreSQL (libpq, dynamic)
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
# Uses system libsqlite3-dev when installed; pass -DSQLITE3_ROOT=<prefix> to
# point at a local/staged copy (headers in <prefix>/include, lib in
# <prefix>/lib) — the repo defaults to build-deps/sqlite for that. Vendoring the
# amalgamation under third_party/ is still an open decision (design §14).
# ---------------------------------------------------------------------------
find_path(SQLITE3_INCLUDE_DIR sqlite3.h
    HINTS ${SQLITE3_ROOT}/include
    PATHS ${CMAKE_SOURCE_DIR}/build-deps/sqlite/include)
find_library(SQLITE3_LIBRARY NAMES sqlite3
    HINTS ${SQLITE3_ROOT}/lib
    PATHS ${CMAKE_SOURCE_DIR}/build-deps/sqlite/lib)
if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
    message(FATAL_ERROR "SQLite not found. Install libsqlite3-dev, or pass "
            "-DSQLITE3_ROOT=<prefix> (headers in <prefix>/include, lib in <prefix>/lib).")
endif()
add_library(SQLite::SQLite3 UNKNOWN IMPORTED)
set_target_properties(SQLite::SQLite3 PROPERTIES
    IMPORTED_LOCATION "${SQLITE3_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SQLITE3_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${CMAKE_DL_LIBS};m")   # static libsqlite3.a needs dl + libm
message(STATUS "cellar: SQLite -> ${SQLITE3_LIBRARY} (include ${SQLITE3_INCLUDE_DIR})")

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

# Embed migrations into the binary (so cellar can bootstrap a DB).
file(GLOB MIG_FILES      CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/sql/migrations/*.sql)
file(GLOB DEMO_MIG_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/sql/demo/*.sql)
file(GLOB TENANCY_MIG_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/sql/tenancy/*.sql)
set(MIGRATIONS_C         ${CMAKE_BINARY_DIR}/generated/sql_migrations.c)
set(DEMO_MIGRATIONS_C    ${CMAKE_BINARY_DIR}/generated/sql_demo_migrations.c)
set(TENANCY_MIGRATIONS_C ${CMAKE_BINARY_DIR}/generated/sql_tenancy_migrations.c)
add_custom_command(
    OUTPUT ${MIGRATIONS_C}
    COMMAND ${CMAKE_COMMAND} -DMIGDIR=${CMAKE_SOURCE_DIR}/sql/migrations
            -DOUTFILE=${MIGRATIONS_C} -DARRAY=CEL_MIGRATIONS
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_migrations.cmake
    DEPENDS ${MIG_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_migrations.cmake
    COMMENT "Embedding sql/migrations (core)" VERBATIM)
add_custom_command(
    OUTPUT ${DEMO_MIGRATIONS_C}
    COMMAND ${CMAKE_COMMAND} -DMIGDIR=${CMAKE_SOURCE_DIR}/sql/demo
            -DOUTFILE=${DEMO_MIGRATIONS_C} -DARRAY=CEL_DEMO_MIGRATIONS
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_migrations.cmake
    DEPENDS ${DEMO_MIG_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_migrations.cmake
    COMMENT "Embedding sql/demo" VERBATIM)
add_custom_command(
    OUTPUT ${TENANCY_MIGRATIONS_C}
    COMMAND ${CMAKE_COMMAND} -DMIGDIR=${CMAKE_SOURCE_DIR}/sql/tenancy
            -DOUTFILE=${TENANCY_MIGRATIONS_C} -DARRAY=CEL_TENANCY_MIGRATIONS
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_migrations.cmake
    DEPENDS ${TENANCY_MIG_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_migrations.cmake
    COMMENT "Embedding sql/tenancy (pooled)" VERBATIM)

file(GLOB_RECURSE CELLAR_SOURCES CONFIGURE_DEPENDS src/*.c)
add_executable(cellar ${CELLAR_SOURCES} ${WEB_ASSETS_C} ${MIGRATIONS_C} ${DEMO_MIGRATIONS_C} ${TENANCY_MIGRATIONS_C})
target_include_directories(cellar PRIVATE src src/core)
# OpenSSL: HMAC-SHA1 for TOTP (RFC 6238) + RS256 verification for OIDC ID tokens.
# Already in-process via portico's TLS / libpq.
find_package(OpenSSL REQUIRED)
# libcurl: fetch OIDC providers' JWKS (public keys) over HTTPS for OAuth sign-in.
# Runtime libcurl.so is near-universal; linked dynamically like libpq.
find_package(CURL REQUIRED)

target_link_libraries(cellar PRIVATE
    portico
    opcode_dispatcher
    logger
    cjson
    SQLite::SQLite3
    PostgreSQL::PostgreSQL
    OpenSSL::Crypto
    CURL::libcurl
    ${SODIUM_LIB}
    ${UUID_LIB}
    Threads::Threads)

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

    # DB-free unit test for the per-app SQLite handle layer (Phase 1 foundation):
    # open-by-file, the connection pool, per-app write serialization, concurrent
    # readers, the get-by-path cache, and isolation between app files. Creates
    # throwaway SQLite files under /tmp; needs no Postgres.
    add_executable(app_db_test tests/app_db_test.c src/core/app_db.c)
    target_include_directories(app_db_test PRIVATE src/core)
    target_link_libraries(app_db_test SQLite::SQLite3 logger Threads::Threads)
    add_test(NAME app_db COMMAND app_db_test)

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

    find_program(PYTHON3 python3)
    if(PYTHON3)
        add_test(NAME smoke
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/run_with_server.py
                    $<TARGET_FILE:cellar> ${CMAKE_SOURCE_DIR}/scripts/smoke_test.py)
        # Mailer (no DB): send via the send-test-mail CLI to a local mock SMTP sink
        # and assert the captured message is well-formed RFC 5322.
        add_test(NAME mailer
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/mailer_test.sh $<TARGET_FILE:cellar>)
        # Migration runner: bootstrap a throwaway DB (needs CREATE DATABASE).
        find_program(PSQL psql)
        if(PSQL)
            add_test(NAME migrate
                COMMAND bash ${CMAKE_SOURCE_DIR}/tests/migrate_test.sh $<TARGET_FILE:cellar>)
            # Pooled multi-tenancy (Model B): two tenants on one server, cross-tenant
            # isolation proven end-to-end (needs CREATE DATABASE + a tenant-scoped table).
            add_test(NAME tenancy
                COMMAND bash ${CMAKE_SOURCE_DIR}/tests/tenancy_test.sh $<TARGET_FILE:cellar>)
            # Postgres RLS defense-in-depth: a raw query is DB-confined to the
            # caller's tenant (needs CREATE DATABASE + CREATE ROLE).
            add_test(NAME rls
                COMMAND bash ${CMAKE_SOURCE_DIR}/tests/rls_test.sh $<TARGET_FILE:cellar>)
            # libpq pipelining (perf lever): the pooled-mode BEGIN/set_config/query/
            # COMMIT goes as one pipelined round trip — proven to preserve tenant
            # isolation AND to advance cel_db_pipelined_txns_total (CEL_DB_PIPELINE=1).
            add_test(NAME pipeline
                COMMAND bash ${CMAKE_SOURCE_DIR}/tests/pipeline_test.sh $<TARGET_FILE:cellar>)
            # Tenant lifecycle + admin tiers: provisioning CLIs, tenant-admin vs
            # platform-admin scoping, suspend/resume gating.
            add_test(NAME tenant_lifecycle
                COMMAND bash ${CMAKE_SOURCE_DIR}/tests/tenant_lifecycle_test.sh $<TARGET_FILE:cellar>)
            # Graduate: export a pooled tenant, load into a fresh standalone (Model A).
            add_test(NAME graduate
                COMMAND bash ${CMAKE_SOURCE_DIR}/tests/graduate_test.sh $<TARGET_FILE:cellar>)
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
            # RPC (#54): a whitelisted SQL function called over REST, with the
            # whitelist proven to block non-whitelisted roles and functions.
            add_test(NAME rpc
                COMMAND bash ${CMAKE_SOURCE_DIR}/tests/rpc_test.sh $<TARGET_FILE:cellar>)
            # RPC tenant seatbelt (H-1/M-3): in pooled mode an anon-whitelisted RPC
            # is still denied for a tenant-less caller (no empty-tenant execution).
            add_test(NAME rpc_tenant
                COMMAND bash ${CMAKE_SOURCE_DIR}/tests/rpc_tenant_test.sh $<TARGET_FILE:cellar>)
            # RPC SECURITY DEFINER audit (H-4): a whitelisted prosecdef function
            # (which bypasses RLS) is warned about at startup; INVOKER is not.
            add_test(NAME rpc_secdef
                COMMAND bash ${CMAKE_SOURCE_DIR}/tests/rpc_secdef_test.sh $<TARGET_FILE:cellar>)
        endif()
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
        # Session cache (#55) end-to-end: a token whose session row was deleted still
        # resolves (served from cache) — proof the cache is on the auth path (needs psql).
        if(PSQL)
            add_test(NAME session_cache_e2e
                COMMAND bash ${CMAKE_SOURCE_DIR}/tests/session_cache_test.sh $<TARGET_FILE:cellar>)
            # Cache-coherence visibility (M-1/M-2): startup + revoke CLI warn about
            # the up-to-TTL revocation/role/tenant propagation latency.
            add_test(NAME cache_coherence
                COMMAND bash ${CMAKE_SOURCE_DIR}/tests/cache_coherence_test.sh $<TARGET_FILE:cellar>)
        endif()
        # Auth rate limit end-to-end: a low per-IP limit makes /auth/login 429.
        add_test(NAME rate_limit_e2e
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/rate_limit_test.sh $<TARGET_FILE:cellar>)
        # Observability (#Phase 9) end-to-end: GET /metrics is a live Prometheus
        # scrape and counters (2xx, latency histogram, failed logins) move with traffic.
        # Bearer-gated (L-3): the wrapper sets CEL_METRICS_TOKEN.
        add_test(NAME metrics_e2e
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/metrics_e2e_test.sh $<TARGET_FILE:cellar>)
        # Prepared-statement cache (perf lever): repeated data-API query shapes
        # PQprepare once per connection then PQexecPrepared — proven via the
        # prepare/reuse counter. Wrapper sets CEL_METRICS_TOKEN to read /metrics.
        add_test(NAME prepared_stmt
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/prepared_stmt_test.sh $<TARGET_FILE:cellar>)
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
