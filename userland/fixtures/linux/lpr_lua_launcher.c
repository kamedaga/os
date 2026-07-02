#include <stddef.h>
#include <stdio.h>

typedef struct lua_State lua_State;

extern lua_State *luaL_newstate(void);
extern void luaL_openlibs(lua_State *L);
extern int luaL_loadfilex(lua_State *L, const char *filename, const char *mode);
extern int lua_pcallk(lua_State *L, int nargs, int nresults, int errfunc, long ctx, void *k);
extern const char *lua_tolstring(lua_State *L, int idx, size_t *len);
extern void lua_close(lua_State *L);

static const char *lua_string(lua_State *L, int idx)
{
    const char *s = lua_tolstring(L, idx, NULL);
    return s != NULL ? s : "(non-string error)";
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fputs("usage: lua.elf script.lua\n", stderr);
        return 2;
    }

    lua_State *L = luaL_newstate();
    if (L == NULL) {
        fputs("lua: failed to create state\n", stderr);
        return 1;
    }

    luaL_openlibs(L);
    int status = luaL_loadfilex(L, argv[1], NULL);
    if (status == 0) {
        status = lua_pcallk(L, 0, -1, 0, 0, NULL);
    }
    if (status != 0) {
        fprintf(stderr, "lua: %s\n", lua_string(L, -1));
        lua_close(L);
        return 1;
    }

    lua_close(L);
    return 0;
}
