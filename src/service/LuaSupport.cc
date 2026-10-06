// Profile and uncached loader adapted from the historical lua-service branch.
#include "ThreadClock.hpp"
#include <optional>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace skynet::service {
namespace {
char kProfileStartRegistryKey;
char kProfileTotalRegistryKey;

[[nodiscard]] double monotonic_seconds() {
    return static_cast<double>(runtime::thread_microseconds()) / 1'000'000.0;
}

void create_weak_key_table(lua_State* state, const void* registry_key) {
    lua_newtable(state);
    lua_newtable(state);
    lua_pushliteral(state, "k");
    lua_setfield(state, -2, "__mode");
    lua_setmetatable(state, -2);
    lua_rawsetp(state, LUA_REGISTRYINDEX, registry_key);
}

void push_registry_table(lua_State* state, const void* registry_key) {
    lua_rawgetp(state, LUA_REGISTRYINDEX, registry_key);
}

int push_profile_thread(lua_State* state) {
    if (lua_gettop(state) == 0) {
        lua_pushthread(state);
    } else {
        lua_settop(state, 1);
        luaL_checktype(state, 1, LUA_TTHREAD);
    }
    return 1;
}

[[nodiscard]] bool read_profile_number(
        lua_State* state,
        const void* registry_key,
        int thread_index,
        double* value) {
    thread_index = lua_absindex(state, thread_index);
    push_registry_table(state, registry_key);
    lua_pushvalue(state, thread_index);
    lua_rawget(state, -2);
    const bool found = lua_isnumber(state, -1);
    if (found) {
        *value = lua_tonumber(state, -1);
    }
    lua_pop(state, 2);
    return found;
}

void write_profile_number(
        lua_State* state,
        const void* registry_key,
        int thread_index,
        std::optional<double> value) {
    thread_index = lua_absindex(state, thread_index);
    push_registry_table(state, registry_key);
    lua_pushvalue(state, thread_index);
    if (value) {
        lua_pushnumber(state, *value);
    } else {
        lua_pushnil(state);
    }
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

int profile_start(lua_State* state) {
    const int thread_index = push_profile_thread(state);
    double ignored = 0;
    if (read_profile_number(
            state, &kProfileStartRegistryKey, thread_index, &ignored)) {
        return luaL_error(state, "thread profile started more than once");
    }
    write_profile_number(state, &kProfileTotalRegistryKey, thread_index, 0.0);
    write_profile_number(
        state, &kProfileStartRegistryKey, thread_index, monotonic_seconds());
    return 0;
}

int profile_stop(lua_State* state) {
    const int thread_index = push_profile_thread(state);
    double start = 0;
    if (!read_profile_number(
            state, &kProfileStartRegistryKey, thread_index, &start)) {
        return luaL_error(state, "call profile.start() before profile.stop()");
    }
    double total = 0;
    (void)read_profile_number(
        state, &kProfileTotalRegistryKey, thread_index, &total);
    if (start > 0) {
        total += monotonic_seconds() - start;
    }
    write_profile_number(
        state, &kProfileStartRegistryKey, thread_index, std::nullopt);
    write_profile_number(
        state, &kProfileTotalRegistryKey, thread_index, std::nullopt);
    lua_pushnumber(state, total);
    return 1;
}

int profile_resume(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTHREAD);
    double start = 0;
    const bool profiled = read_profile_number(
        state, &kProfileStartRegistryKey, 1, &start);
    if (profiled) {
        write_profile_number(
            state, &kProfileStartRegistryKey, 1, monotonic_seconds());
    }
    const int argument_count = lua_gettop(state);
    // Keep the thread on this protected Lua stack, not in a registry reference
    // that would leak if resume raises an allocation error via longjmp.
    lua_pushvalue(state, 1);
    lua_insert(state, 1);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 2);
    lua_call(state, argument_count, LUA_MULTRET);

    const int result_count = lua_gettop(state) - 1;
    const int thread_index = 1;
    if (read_profile_number(
            state, &kProfileStartRegistryKey, thread_index, &start) && start > 0) {
        double total = 0;
        (void)read_profile_number(
            state, &kProfileTotalRegistryKey, thread_index, &total);
        total += monotonic_seconds() - start;
        write_profile_number(
            state, &kProfileTotalRegistryKey, thread_index, total);
        write_profile_number(
            state, &kProfileStartRegistryKey, thread_index, 0.0);
    }
    lua_remove(state, 1);
    return result_count;
}

int profile_wrapped_resume(lua_State* state) {
    const int argument_count = lua_gettop(state);
    lua_pushvalue(state, lua_upvalueindex(2));
    lua_pushcclosure(state, profile_resume, 1);
    lua_insert(state, 1);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 2);
    lua_call(state, argument_count + 1, LUA_MULTRET);

    if (!lua_toboolean(state, 1)) {
        lua_remove(state, 1);
        return lua_error(state);
    }
    lua_remove(state, 1);
    return lua_gettop(state);
}

int profile_wrap(lua_State* state) {
    luaL_checktype(state, 1, LUA_TFUNCTION);
    lua_State* coroutine = lua_newthread(state);
    lua_pushvalue(state, 1);
    lua_xmove(state, coroutine, 1);
    lua_pushvalue(state, -1);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushcclosure(state, profile_wrapped_resume, 2);
    return 1;
}

int open_profile(lua_State* state) {
    create_weak_key_table(state, &kProfileStartRegistryKey);
    create_weak_key_table(state, &kProfileTotalRegistryKey);

    lua_newtable(state);
    lua_pushcfunction(state, profile_start);
    lua_setfield(state, -2, "start");
    lua_pushcfunction(state, profile_stop);
    lua_setfield(state, -2, "stop");

    lua_getglobal(state, "coroutine");
    lua_getfield(state, -1, "resume");
    lua_pushvalue(state, -1);
    lua_pushcclosure(state, profile_resume, 1);
    lua_setfield(state, -4, "resume");
    lua_pushcclosure(state, profile_wrap, 1);
    lua_setfield(state, -3, "wrap");
    lua_pop(state, 1);
    return 1;
}

int no_operation(lua_State*) {
    return 0;
}

int direct_load_file(lua_State* state) {
    const char* file_name = luaL_optstring(state, 1, nullptr);
    const char* mode = luaL_optstring(state, 2, "bt");
    const int environment_index = lua_isnone(state, 3) ? 0 : 3;
    const int status = luaL_loadfilex_(state, file_name, mode);
    if (status != LUA_OK) {
        lua_pushnil(state);
        lua_insert(state, -2);
        return 2;
    }
    if (environment_index != 0) {
        lua_pushvalue(state, environment_index);
        if (lua_setupvalue(state, -2, 1) == nullptr) {
            lua_pop(state, 1);
        }
    }
    return 1;
}

int direct_lua_searcher(lua_State* state) {
    luaL_checkstring(state, 1);
    lua_getglobal(state, "package");
    const int package_index = lua_gettop(state);
    lua_getfield(state, package_index, "searchpath");
    lua_pushvalue(state, 1);
    lua_getfield(state, package_index, "path");
    lua_call(state, 2, 2);
    if (lua_isnil(state, -2)) {
        lua_remove(state, -2);
        return 1;
    }

    lua_pop(state, 1);
    const char* file_name = lua_tostring(state, -1);
    if (luaL_loadfilex_(state, file_name, nullptr) != LUA_OK) {
        return luaL_error(
            state,
            "error loading module '%s' from file '%s':\n\t%s",
            lua_tostring(state, 1),
            file_name,
            lua_tostring(state, -1));
    }
    lua_pushvalue(state, -2);
    return 2;
}

void install_direct_lua_loader(lua_State* state) {
    lua_pushcfunction(state, direct_load_file);
    lua_setglobal(state, "loadfile");
    lua_getglobal(state, "package");
    lua_getfield(state, -1, "searchers");
    lua_pushcfunction(state, direct_lua_searcher);
    lua_rawseti(state, -2, 2);
    lua_pop(state, 2);
}

int open_code_cache(lua_State* state) {
    static constexpr luaL_Reg kFunctions[] = {
        {"clear", no_operation},
        {"mode", no_operation},
        {nullptr, nullptr},
    };
    luaL_newlib(state, kFunctions);
    lua_pushcfunction(state, direct_load_file);
    lua_setfield(state, -2, "loadfile");
    return 1;
}

}

void install_lua_support(lua_State* state) {
    install_direct_lua_loader(state);
    luaL_requiref(state, "skynet.profile", open_profile, 0);
    lua_getglobal(state, "coroutine");
    lua_getfield(state, -2, "resume");
    lua_setfield(state, -2, "resume");
    lua_getfield(state, -2, "wrap");
    lua_setfield(state, -2, "wrap");
    lua_pop(state, 2);
    luaL_requiref(state, "skynet.codecache", open_code_cache, 0);
    lua_pop(state, 1);
}
}  // namespace skynet::service
