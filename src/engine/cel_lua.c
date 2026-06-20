/* cellar — LuaJIT hook VM (design §7-9). See cel_lua.h. */
#include "cel_lua.h"

#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

struct cel_lua {
    lua_State *L;
};

cel_lua_t *cel_lua_open(void) {
    cel_lua_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->L = luaL_newstate();
    if (!h->L) { free(h); return NULL; }
    luaL_openlibs(h->L);   /* base + string/table/math/... + ffi + jit */
    return h;
}

void cel_lua_close(cel_lua_t *h) {
    if (!h) return;
    if (h->L) lua_close(h->L);
    free(h);
}

int cel_lua_dostring(cel_lua_t *h, const char *src, char *errbuf, size_t errlen) {
    if (errbuf && errlen) errbuf[0] = '\0';
    if (!h || !h->L) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "no lua state");
        return -1;
    }
    /* luaL_loadstring compiles to a chunk on the stack; lua_pcall runs it. Both
     * report failure by leaving an error string on the stack — neither longjmps
     * past this frame, so a hook fault can't take down the worker. */
    if (luaL_loadstring(h->L, src) != 0 || lua_pcall(h->L, 0, 0, 0) != 0) {
        const char *msg = lua_tostring(h->L, -1);
        if (errbuf && errlen) snprintf(errbuf, errlen, "%s", msg ? msg : "lua error");
        lua_pop(h->L, 1);
        return -1;
    }
    return 0;
}
