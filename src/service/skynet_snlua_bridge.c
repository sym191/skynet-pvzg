#include "skynet_snlua_bridge.h"

// Keep the SNLua implementation source-compatible with the vendored Skynet
// revision. Static helpers from the implementation are used below to provide
// a synchronous Service::init entry point for the C++ runtime.
#include "../../third_party/skynet/service-src/service_snlua.c"

LUAMOD_API int luaopen_skynet_core(lua_State* L);

static void
preload_skynet_core(lua_State* L) {
	luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
	lua_pushcfunction(L, luaopen_skynet_core);
	lua_setfield(L, -2, "skynet.core");
	lua_pop(L, 1);
}

static int
init_direct(struct snlua* service, struct skynet_context* context, const char* args, size_t size) {
	lua_State* L = service->L;
	service->ctx = context;
	lua_gc(L, LUA_GCSTOP, 0);
	lua_pushboolean(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
	luaL_openlibs(L);
	preload_skynet_core(L);
	luaL_requiref(L, "skynet.profile", init_profile, 0);

	int profile_lib = lua_gettop(L);
	lua_getglobal(L, "coroutine");
	lua_getfield(L, profile_lib, "resume");
	lua_setfield(L, -2, "resume");
	lua_getfield(L, profile_lib, "wrap");
	lua_setfield(L, -2, "wrap");
	lua_settop(L, profile_lib - 1);

	lua_pushlightuserdata(L, context);
	lua_setfield(L, LUA_REGISTRYINDEX, "skynet_context");
	luaL_requiref(L, "skynet.codecache", codecache, 0);
	lua_pop(L, 1);
	lua_gc(L, LUA_GCGEN, 0, 0);

	const char* path = optstring(context, "lua_path", "./lualib/?.lua;./lualib/?/init.lua");
	lua_pushstring(L, path);
	lua_setglobal(L, "LUA_PATH");
	const char* cpath = optstring(context, "lua_cpath", "./luaclib/?.so");
	lua_pushstring(L, cpath);
	lua_setglobal(L, "LUA_CPATH");
	const char* service_path = optstring(context, "luaservice", "./service/?.lua");
	lua_pushstring(L, service_path);
	lua_setglobal(L, "LUA_SERVICE");
	const char* preload = skynet_command(context, "GETENV", "preload");
	lua_pushstring(L, preload);
	lua_setglobal(L, "LUA_PRELOAD");

	lua_pushcfunction(L, traceback);
	const char* loader = optstring(context, "lualoader", "./lualib/loader.lua");
	int result = luaL_loadfile(L, loader);
	if (result != LUA_OK) {
		skynet_error(context, "Can't load %s : %s", loader, lua_tostring(L, -1));
		report_launcher_error(context);
		return 1;
	}
	lua_pushlstring(L, args, size);
	result = lua_pcall(L, 1, 0, 1);
	if (result != LUA_OK) {
		skynet_error(context, "lua loader error : %s", lua_tostring(L, -1));
		report_launcher_error(context);
		return 1;
	}
	lua_settop(L, 0);
	if (lua_getfield(L, LUA_REGISTRYINDEX, "memlimit") == LUA_TNUMBER) {
		size_t limit = (size_t)lua_tointeger(L, -1);
		service->mem_limit = limit;
		skynet_error(context, "Set memory limit to %.2f M", (float)limit / (1024 * 1024));
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, "memlimit");
	}
	lua_pop(L, 1);
	lua_gc(L, LUA_GCRESTART, 0);
	return 0;
}

struct snlua*
skynet_snlua_create(void) {
	return snlua_create();
}

void
skynet_snlua_release(struct snlua* service) {
	if (service != NULL) {
		snlua_release(service);
	}
}

int
skynet_snlua_init_direct(
		struct snlua* service,
		struct skynet_context* context,
		const char* arguments,
		size_t size) {
	if (service == NULL || service->L == NULL || context == NULL || arguments == NULL) {
		return 1;
	}
	return init_direct(service, context, arguments, size);
}

void
skynet_snlua_signal(struct snlua* service, int signal) {
	if (service != NULL && service->ctx != NULL) {
		snlua_signal(service, signal);
	}
}

lua_State*
skynet_snlua_state(struct snlua* service) {
	return service == NULL ? NULL : service->L;
}

size_t
skynet_snlua_memory(struct snlua* service) {
	return service == NULL ? 0 : service->mem;
}

size_t
skynet_snlua_memory_limit(struct snlua* service) {
	return service == NULL ? 0 : service->mem_limit;
}
