#include "LuaService.hpp"

#include "Context.hpp"
#include "Runtime.hpp"

#include <cstdlib>
#include <limits>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
int luaopen_skynet_core(lua_State*);
int luaopen_skynet_socketdriver(lua_State*);
int luaopen_skynet_netpack(lua_State*);
}

namespace skynet::service {
void install_lua_support(lua_State* state);
namespace {
// Validate address blob lengths before the upstream C functions dereference
// their pointer-only ABI, and reject a failed bind before socket.lua suspends.
int checked_socket_call(lua_State* state) {
    const int kind = static_cast<int>(lua_tointeger(state, lua_upvalueindex(2)));
    if (kind != 0) {
        std::size_t size;
        const char* address = luaL_checklstring(state, kind == 1 ? 2 : 1, &size);
        if (size != 7 || static_cast<unsigned char>(address[0]) != 1) {
            return luaL_error(state, "Invalid IPv4 UDP address");
        }
    }
    const int arguments = lua_gettop(state);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    lua_call(state, arguments, LUA_MULTRET);
    if (kind == 0 && lua_tointeger(state, 1) < 0) { return luaL_error(state, "socket bind failed"); }
    return lua_gettop(state);
}
int open_socket_driver(lua_State* state) {
    luaopen_skynet_socketdriver(state);
    int kind = 0;
    for (const char* name : {"bind", "udp_send", "udp_address"}) {
        lua_getfield(state, -1, name);
        lua_pushinteger(state, kind++);
        lua_pushcclosure(state, checked_socket_call, 2);
        lua_setfield(state, -2, name);
    }
    return 1;
}
}  // namespace

LuaService::~LuaService() { if (state_) { lua_close(state_); } }

void* LuaService::allocate(void* user, void* pointer, std::size_t old_size, std::size_t new_size) {
    auto& self = *static_cast<LuaService*>(user);
    const auto base = self.memory_.load(std::memory_order_relaxed) - (pointer ? old_size : 0);
    if (new_size == 0) {
        std::free(pointer);
        self.memory_.store(base, std::memory_order_relaxed);
        return nullptr;
    }
    if (new_size > std::numeric_limits<std::size_t>::max() - base ||
        (self.memory_limit_ && base + new_size > self.memory_limit_ && (!pointer || new_size > old_size))) {
        return nullptr;
    }
    auto* result = std::realloc(pointer, new_size);
    if (result) { self.memory_.store(base + new_size, std::memory_order_relaxed); }
    return result;
}

void LuaService::interrupt_hook(lua_State* state, lua_Debug*) {
    auto* self = *static_cast<LuaService**>(lua_getextraspace(state));
    if (self->interrupt_.exchange(false, std::memory_order_relaxed)) {
        luaL_error(state, "signal 0");
    }
}

bool LuaService::init(runtime::Context& context, std::string_view arguments) {
    context_ = &context;
    // Match SNLua: loading code is the first actor message, never the caller's stack.
    return context.send(context.handle(), 0, 0,
        std::as_bytes(std::span(arguments.data(), arguments.size()))) >= 0;
}

int LuaService::initialize_lua(lua_State* state) {
    auto* self = *static_cast<LuaService**>(lua_getextraspace(state));
    lua_pushboolean(state, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, "LUA_NOENV");
    luaL_openlibs(state);
    install_lua_support(state);
    lua_pushlightuserdata(state, self->context_);
    lua_setfield(state, LUA_REGISTRYINDEX, "skynet_context");
    luaL_getsubtable(state, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
    lua_pushcfunction(state, luaopen_skynet_core);
    lua_setfield(state, -2, "skynet.core");
    lua_pushcfunction(state, open_socket_driver);
    lua_setfield(state, -2, "skynet.socketdriver");
    lua_pushcfunction(state, luaopen_skynet_netpack);
    lua_setfield(state, -2, "skynet.netpack");
    lua_pop(state, 1);
    for (const auto& [name, value] : self->globals_) {
        lua_pushlstring(state, value.data(), value.size());
        lua_setglobal(state, name.c_str());
    }
    if (luaL_loadfilex_(state, self->loader_.c_str(), nullptr) != LUA_OK) { return lua_error(state); }
    lua_pushlstring(state, self->arguments_.data(), self->arguments_.size());
    lua_call(state, 1, 0);
    if (lua_getfield(state, LUA_REGISTRYINDEX, "memlimit") == LUA_TNUMBER) {
        const auto limit = lua_tointeger(state, -1);
        if (limit > 0) { self->memory_limit_ = static_cast<std::size_t>(limit); }
    }
    lua_pop(state, 1);
    lua_pushnil(state);
    lua_setfield(state, LUA_REGISTRYINDEX, "memlimit");
    return 0;
}

int LuaService::dispatch_lua(lua_State* state) {
    auto* self = *static_cast<LuaService**>(lua_getextraspace(state));
    self->reserved_message_ = self->context_->invoke_native(*self->current_message_);
    return 0;
}

void LuaService::dispatch(runtime::Context& context, runtime::Message message) {
    if (state_) {
        current_message_ = &message;
        reserved_message_ = false;
        lua_pushcfunction(state_, dispatch_lua);
        if (lua_pcall(state_, 0, 0, 0) != LUA_OK) {
            context.log(lua_tostring(state_, -1) ? lua_tostring(state_, -1) : "Lua callback failed");
            lua_pop(state_, 1);
        }
        if (reserved_message_) { (void)message.data.release(); }
        current_message_ = nullptr;
        return;
    }
    arguments_ = message.string_payload();
    auto& env = context.runtime().environment();
    globals_ = {
        {"LUA_PATH", env.get("lua_path").value_or("./lualib/?.lua;./lualib/?/init.lua")},
        {"LUA_CPATH", env.get("lua_cpath").value_or("./luaclib/?.so")},
        {"LUA_SERVICE", env.get("luaservice").value_or("./service/?.lua")},
    };
    if (auto preload = env.get("preload")) { globals_.emplace_back("LUA_PRELOAD", *preload); }
    loader_ = env.get("lualoader").value_or("./lualib/loader.lua");
    state_ = lua_newstate(allocate, this, 1);
    bool success = false;
    if (state_) {
        *static_cast<LuaService**>(lua_getextraspace(state_)) = this;
        // New coroutines inherit this hook. The foreign signal thread only touches an atomic.
        lua_sethook(state_, interrupt_hook, LUA_MASKCOUNT, 1000);
        lua_pushcfunction(state_, initialize_lua);
        success = lua_pcall(state_, 0, 0, 0) == LUA_OK;
        if (!success) { context.log(lua_tostring(state_, -1) ? lua_tostring(state_, -1) : "Lua allocation failed"); }
        lua_settop(state_, 0);
    }
    if (!success) {
        constexpr std::string_view error = "ERROR";
        (void)context.send(".launcher", 0, 0, std::as_bytes(std::span(error.data(), error.size())));
        (void)context.command("EXIT");
        context.runtime().initialization_failed(context.handle());
    }
}

void LuaService::signal(runtime::Context& context, int value) {
    if (value == 0) { interrupt_.store(true, std::memory_order_relaxed); }
    if (value == 1) { context.log("Current Memory " + std::to_string(memory_.load() / 1024.0) + "K"); }
}
}  // namespace skynet::service
