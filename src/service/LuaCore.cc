#include "LuaCore.hpp"

#include "Context.hpp"
#include "LuaRuntime.hpp"
#include "LuaSerialization.hpp"
#include "Message.hpp"
#include "lua_api.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace skynet::service::detail {
namespace {

constexpr int kTypeMask = 0xff;
const auto kClockStart = std::chrono::steady_clock::now();

[[nodiscard]] LuaRuntime* RequireRuntime(lua_State* state) {
    LuaRuntime* runtime = LuaRuntime::FromState(state);
    if (runtime == nullptr) {
        luaL_error(state, "initialize Lua service context first");
    }
    return runtime;
}

[[nodiscard]] std::span<const std::byte> AsBytes(
        const void* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return {};
    }
    return {static_cast<const std::byte*>(data), size};
}

int LuaCommand(lua_State* state) {
    LuaRuntime* runtime = RequireRuntime(state);
    const char* command = luaL_checkstring(state, 1);
    std::optional<std::string_view> parameter;
    if (lua_gettop(state) == 2) {
        std::size_t size = 0;
        const char* value = luaL_checklstring(state, 2, &size);
        parameter = std::string_view(value, size);
    }
    const auto result = runtime->context().command(command, parameter);
    if (!result) {
        return 0;
    }
    lua_pushlstring(state, result->data(), result->size());
    return 1;
}

int LuaAddressCommand(lua_State* state) {
    LuaRuntime* runtime = RequireRuntime(state);
    const char* command = luaL_checkstring(state, 1);
    std::optional<std::string_view> parameter;
    if (lua_gettop(state) == 2) {
        std::size_t size = 0;
        const char* value = luaL_checklstring(state, 2, &size);
        parameter = std::string_view(value, size);
    }
    const auto result = runtime->context().command(command, parameter);
    if (!result || result->size() < 2 || result->front() != ':') {
        return 0;
    }
    runtime::Handle address = 0;
    const auto [position, error] = std::from_chars(
        result->data() + 1, result->data() + result->size(), address, 16);
    if (error != std::errc{} || position != result->data() + result->size()) {
        return 0;
    }
    lua_pushinteger(state, address);
    return 1;
}

int LuaIntegerCommand(lua_State* state) {
    LuaRuntime* runtime = RequireRuntime(state);
    const char* command = luaL_checkstring(state, 1);
    std::optional<std::string> owned_parameter;
    std::optional<std::string_view> parameter;
    if (lua_gettop(state) == 2) {
        if (lua_type(state, 2) == LUA_TNUMBER) {
            owned_parameter = std::to_string(
                static_cast<std::int32_t>(luaL_checkinteger(state, 2)));
            parameter = *owned_parameter;
        } else {
            std::size_t size = 0;
            const char* value = luaL_checklstring(state, 2, &size);
            parameter = std::string_view(value, size);
        }
    }
    const auto result = runtime->context().command(command, parameter);
    if (!result) {
        return 0;
    }

    char* end = nullptr;
    const long long integer = std::strtoll(result->c_str(), &end, 0);
    if (end != nullptr && end == result->c_str() + result->size()) {
        lua_pushinteger(state, static_cast<lua_Integer>(integer));
        return 1;
    }

    end = nullptr;
    const double real = std::strtod(result->c_str(), &end);
    if (end == nullptr || end != result->c_str() + result->size()) {
        return luaL_error(state, "invalid numeric command result: %s", result->c_str());
    }
    lua_pushnumber(state, real);
    return 1;
}

int LuaGenerateSession(lua_State* state) {
    lua_pushinteger(state, RequireRuntime(state)->context().new_session());
    return 1;
}

int SendMessage(lua_State* state, runtime::Handle source, int type_index) {
    LuaRuntime* runtime = RequireRuntime(state);

    runtime::Handle destination_handle = 0;
    std::optional<std::string_view> destination_name;
    if (lua_type(state, 1) == LUA_TNUMBER) {
        destination_handle = static_cast<runtime::Handle>(
            luaL_checkinteger(state, 1));
        if (destination_handle == 0) {
            return luaL_error(state, "invalid service address 0");
        }
    } else {
        std::size_t size = 0;
        const char* value = luaL_checklstring(state, 1, &size);
        destination_name = std::string_view(value, size);
    }

    const int type = static_cast<int>(luaL_checkinteger(state, type_index));
    const bool allocate_session = lua_isnil(state, type_index + 1);
    int session = allocate_session
        ? 0
        : static_cast<int>(luaL_checkinteger(state, type_index + 1));

    const int payload_index = type_index + 2;
    const void* payload = nullptr;
    std::size_t payload_size = 0;
    bool owns_payload = false;
    if (lua_type(state, payload_index) == LUA_TSTRING) {
        payload = lua_tolstring(state, payload_index, &payload_size);
    } else if (lua_type(state, payload_index) == LUA_TLIGHTUSERDATA) {
        payload = lua_touserdata(state, payload_index);
        const lua_Integer requested_size =
            luaL_checkinteger(state, payload_index + 1);
        if (requested_size < 0) {
            return luaL_error(state, "message size cannot be negative");
        }
        payload_size = static_cast<std::size_t>(requested_size);
        owns_payload = true;
    } else {
        return luaL_error(
            state,
            "invalid message payload type %s",
            lua_typename(state, lua_type(state, payload_index)));
    }

    if (destination_name) {
        session = runtime->context().send(
            *destination_name,
            source,
            type & kTypeMask,
            session,
            AsBytes(payload, payload_size),
            allocate_session);
    } else {
        session = runtime->context().send(
            destination_handle,
            source,
            type & kTypeMask,
            session,
            AsBytes(payload, payload_size),
            allocate_session);
    }
    if (owns_payload) {
        std::free(const_cast<void*>(payload));
    }
    if (session < 0) {
        return 0;
    }
    lua_pushinteger(state, session);
    return 1;
}

int LuaSend(lua_State* state) {
    return SendMessage(state, 0, 2);
}

int LuaRedirect(lua_State* state) {
    const runtime::Handle source = static_cast<runtime::Handle>(
        luaL_checkinteger(state, 2));
    return SendMessage(state, source, 3);
}

int LuaLog(lua_State* state) {
    LuaRuntime* runtime = RequireRuntime(state);
    const int count = lua_gettop(state);
    luaL_Buffer buffer;
    luaL_buffinit(state, &buffer);
    for (int index = 1; index <= count; ++index) {
        luaL_tolstring(state, index, nullptr);
        luaL_addvalue(&buffer);
        if (index != count) {
            luaL_addchar(&buffer, ' ');
        }
    }
    luaL_pushresult(&buffer);
    std::size_t size = 0;
    const char* message = lua_tolstring(state, -1, &size);
    runtime->context().log(std::string_view(message, size));
    return 0;
}

int LuaPointerToString(lua_State* state) {
    if (lua_isnoneornil(state, 1)) {
        return 0;
    }
    const lua_Integer requested_size = luaL_checkinteger(state, 2);
    if (requested_size < 0) {
        return luaL_error(state, "message size cannot be negative");
    }
    const void* data = lua_touserdata(state, 1);
    if (data == nullptr && requested_size != 0) {
        return luaL_error(state, "message pointer is null");
    }
    lua_pushlstring(
        state,
        static_cast<const char*>(data),
        static_cast<std::size_t>(requested_size));
    return 1;
}

int LuaHarbor(lua_State* state) {
    LuaRuntime* runtime = RequireRuntime(state);
    const runtime::Handle handle = static_cast<runtime::Handle>(
        luaL_checkinteger(state, 1));
    int harbor = 0;
    const bool remote = runtime->context().is_remote(handle, &harbor);
    lua_pushinteger(state, harbor);
    lua_pushboolean(state, remote);
    return 2;
}

int LuaCallback(lua_State* state) {
    luaL_checktype(state, 1, LUA_TFUNCTION);
    const bool forward = lua_toboolean(state, 2) != 0;
    RequireRuntime(state)->SetCallback(state, 1, forward);
    return 0;
}

int LuaPackString(lua_State* state) {
    const int result_count = LuaPack(state);
    if (result_count != 2) {
        return result_count;
    }
    void* data = lua_touserdata(state, -2);
    const std::size_t size = static_cast<std::size_t>(lua_tointeger(state, -1));
    lua_pushlstring(state, static_cast<const char*>(data), size);
    std::free(data);
    return 1;
}

int LuaTrash(lua_State* state) {
    if (lua_type(state, 1) == LUA_TSTRING) {
        return 0;
    }
    if (lua_type(state, 1) != LUA_TLIGHTUSERDATA) {
        return luaL_error(
            state,
            "skynet.trash invalid parameter %s",
            lua_typename(state, lua_type(state, 1)));
    }
    (void)luaL_checkinteger(state, 2);
    std::free(lua_touserdata(state, 1));
    return 0;
}

int LuaNow(lua_State* state) {
    const auto elapsed = std::chrono::steady_clock::now() - kClockStart;
    const auto ticks =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() /
        10;
    lua_pushinteger(state, static_cast<lua_Integer>(ticks));
    return 1;
}

int LuaHighPerformanceCounter(lua_State* state) {
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    lua_pushinteger(state, static_cast<lua_Integer>(nanoseconds.count()));
    return 1;
}

int LuaTrace(lua_State* state) {
    LuaRuntime* runtime = RequireRuntime(state);
    std::size_t tag_size = 0;
    std::size_t text_size = 0;
    const char* tag = luaL_checklstring(state, 1, &tag_size);
    const char* text = luaL_checklstring(state, 2, &text_size);
    std::string message;
    message.reserve(tag_size + text_size + 32);
    message.append("<TRACE ");
    message.append(tag, tag_size);
    message.append("> ");
    message.append(text, text_size);
    runtime->context().log(message);
    return 0;
}

}  // namespace

int OpenLuaCore(lua_State* state) {
    luaL_checkversion(state);
    if (LuaRuntime::FromState(state) == nullptr) {
        return luaL_error(state, "initialize Lua service context first");
    }

    static constexpr luaL_Reg kFunctions[] = {
        {"send", LuaSend},
        {"genid", LuaGenerateSession},
        {"redirect", LuaRedirect},
        {"command", LuaCommand},
        {"intcommand", LuaIntegerCommand},
        {"addresscommand", LuaAddressCommand},
        {"error", LuaLog},
        {"harbor", LuaHarbor},
        {"callback", LuaCallback},
        {"trace", LuaTrace},
        {"tostring", LuaPointerToString},
        {"pack", LuaPack},
        {"unpack", LuaUnpack},
        {"packstring", LuaPackString},
        {"trash", LuaTrash},
        {"now", LuaNow},
        {"hpc", LuaHighPerformanceCounter},
        {nullptr, nullptr},
    };
    luaL_newlib(state, kFunctions);
    return 1;
}

}  // namespace skynet::service::detail
