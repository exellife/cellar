/* cellar — LuaJIT hook VM foundation test (Phase 2).
 *
 * Proves the lifecycle the hook layer rests on: a state opens, runs Lua, isolates
 * compile AND runtime errors (no crash), and has FFI available in the embedded VM
 * (the design's no-binding-glue premise). Calls libc strlen via FFI so the test
 * needs no exported cellar symbols; FFI-into-cellar's-own-API is exercised once
 * real hooks are wired. No server/DB needed. */
#include "cel_lua.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
static void check(const char *name, int cond, const char *detail) {
    printf("  %-4s %-34s %s\n", cond ? "ok" : "FAIL", name, detail ? detail : "");
    if (!cond) fails++;
}

int main(void) {
    cel_lua_t *L = cel_lua_open();
    check("open state", L != NULL, "");
    if (!L) return 1;

    char err[256];

    /* happy path */
    int rc = cel_lua_dostring(L, "local x = 1 + 1; assert(x == 2)", err, sizeof err);
    check("run valid chunk -> 0", rc == 0, err);

    /* compile error is caught, message captured, no crash */
    rc = cel_lua_dostring(L, "this is ) not lua", err, sizeof err);
    check("compile error -> non-zero", rc != 0, err);
    check("compile error message captured", err[0] != '\0', "");

    /* runtime error is caught and the message propagates the reason */
    rc = cel_lua_dostring(L, "error('boom-42')", err, sizeof err);
    check("runtime error -> non-zero", rc != 0, "");
    check("runtime error message has reason", strstr(err, "boom-42") != NULL, err);

    /* the VM survives a faulted chunk and still runs the next one */
    rc = cel_lua_dostring(L, "assert(true)", err, sizeof err);
    check("state usable after error", rc == 0, err);

    /* FFI is open: cdef a libc function and call it (the no-glue premise) */
    rc = cel_lua_dostring(L,
        "local ffi = require('ffi')\n"
        "ffi.cdef[[ unsigned long strlen(const char *s); ]]\n"
        "assert(tonumber(ffi.C.strlen('hello')) == 5)\n",
        err, sizeof err);
    check("FFI call into libc works", rc == 0, err);

    cel_lua_close(L);
    cel_lua_close(NULL);   /* close(NULL) is a no-op */
    check("close (incl. NULL)", 1, "");

    printf("\n%s  (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
