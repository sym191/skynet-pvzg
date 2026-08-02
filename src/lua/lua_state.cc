#include "lua_state.hpp"

#include "lua_api.hpp"

#include <cstddef>
#include <string>

namespace skynet::lua {

namespace {

constexpr const char* kLuaStateInitError = "failed to create Lua state";

bool report_uninitialized_state(std::string* error) {
    if (error != nullptr) {
        *error = kLuaStateInitError;
    }
    return false;
}

}  // namespace

LuaState::LuaState() : state_(luaL_newstate()) {
    if (state_ != nullptr) {
        luaL_openlibs(state_);
    }
}

LuaState::~LuaState() {
    if (state_ != nullptr) {
        lua_close(state_);
    }
}

lua_State* LuaState::get() const noexcept {
    return state_;
}

bool LuaState::execute(
        std::string_view code,
        std::string_view chunk_name,
        std::string* error) {
    if (state_ == nullptr) {
        return report_uninitialized_state(error);
    }

    const int stack_top = lua_gettop(state_);
    const std::string chunk_name_string(chunk_name);
    if (luaL_loadbuffer(
                state_,
                code.data(),
                code.size(),
                chunk_name_string.c_str()) != LUA_OK ||
            lua_pcall(state_, 0, LUA_MULTRET, 0) != LUA_OK) {
        capture_error(error);
        lua_settop(state_, stack_top);
        return false;
    }

    lua_settop(state_, stack_top);
    return true;
}

bool LuaState::execute_file(std::string_view path, std::string* error) {
    if (state_ == nullptr) {
        return report_uninitialized_state(error);
    }

    const int stack_top = lua_gettop(state_);
    const std::string path_string(path);
    if (luaL_loadfile(state_, path_string.c_str()) != LUA_OK ||
            lua_pcall(state_, 0, LUA_MULTRET, 0) != LUA_OK) {
        capture_error(error);
        lua_settop(state_, stack_top);
        return false;
    }

    lua_settop(state_, stack_top);
    return true;
}

void LuaState::set_global_string(std::string_view name, std::string_view value) {
    if (state_ == nullptr) {
        return;
    }

    const std::string name_string(name);
    lua_pushlstring(state_, value.data(), value.size());
    lua_setglobal(state_, name_string.c_str());
}

std::optional<std::string> LuaState::get_global_string(std::string_view name) const {
    if (state_ == nullptr) {
        return std::nullopt;
    }

    const std::string name_string(name);
    lua_getglobal(state_, name_string.c_str());
    if (lua_type(state_, -1) != LUA_TSTRING) {
        lua_pop(state_, 1);
        return std::nullopt;
    }

    std::size_t size = 0;
    const char* value = lua_tolstring(state_, -1, &size);
    std::string result(value, size);
    lua_pop(state_, 1);
    return result;
}

void LuaState::capture_error(std::string* error) {
    if (error != nullptr) {
        const char* message = lua_tostring(state_, -1);
        *error = message != nullptr ? message : "unknown Lua error";
    }
    lua_pop(state_, 1);
}

}  // namespace skynet::lua
