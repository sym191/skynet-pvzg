#include "runtime_config.hpp"

#include <charconv>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace skynet::config {
namespace {

constexpr std::string_view kLoadConfig = R"lua(
local result = {}
local function getenv(name)
    return assert(os.getenv(name), "os.getenv() failed: " .. name)
end
local sep = package.config:sub(1, 1)
local current_path = "." .. sep
local function include(filename)
    local last_path = current_path
    local path, name = filename:match("(.*" .. sep .. ")(.*)$")
    if path then
        if path:sub(1, 1) == sep then
            current_path = path
        else
            current_path = current_path .. path
        end
    else
        name = filename
    end
    local file = assert(io.open(current_path .. name))
    local code = assert(file:read("*a"))
    code = string.gsub(code, "%$([%w_%d]+)", getenv)
    file:close()
    assert(load(code, "@" .. filename, "t", result))()
    current_path = last_path
end
setmetatable(result, { __index = { include = include } })
local config_name = ...
include(config_name)
setmetatable(result, nil)
return result
)lua";

struct LuaStateDeleter {
    void operator()(lua_State* state) const noexcept {
        if (state != nullptr) {
            lua_close(state);
        }
    }
};

using LuaStatePtr = std::unique_ptr<lua_State, LuaStateDeleter>;

[[nodiscard]] ConfigError lua_error(lua_State* state, std::string_view prefix) {
    const char* detail = lua_tostring(state, -1);
    std::string message(prefix);
    if (detail != nullptr) {
        message.append(": ");
        message.append(detail);
    }
    return ConfigError{.message = std::move(message)};
}

[[nodiscard]] std::expected<std::string, ConfigError> scalar_to_string(
    lua_State* state,
    int index,
    std::string_view key
) {
    switch (lua_type(state, index)) {
    case LUA_TSTRING: {
        std::size_t size = 0;
        const char* value = lua_tolstring(state, index, &size);
        return std::string(value, size);
    }
    case LUA_TNUMBER: {
        std::size_t size = 0;
        const char* value = lua_tolstring(state, index, &size);
        return std::string(value, size);
    }
    case LUA_TBOOLEAN:
        return std::string(lua_toboolean(state, index) != 0 ? "true" : "false");
    default:
        return std::unexpected(ConfigError{
            .message = "config value for '" + std::string(key) +
                "' must be a string, number, or boolean",
        });
    }
}

[[nodiscard]] std::expected<std::size_t, ConfigError> parse_thread_count(
    std::string_view value
) {
    std::size_t result = 0;
    const auto [position, error] = std::from_chars(
        value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || position != value.data() + value.size() ||
        result == 0 || result > 1024) {
        return std::unexpected(ConfigError{
            .message = "thread must be an integer in [1, 1024]",
        });
    }
    return result;
}

[[nodiscard]] std::expected<std::uint8_t, ConfigError> parse_harbor_id(
    std::string_view value
) {
    unsigned int result = 0;
    const auto [position, error] = std::from_chars(
        value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || position != value.data() + value.size() ||
        result > std::numeric_limits<std::uint8_t>::max()) {
        return std::unexpected(ConfigError{
            .message = "harbor must be an integer in [0, 255]",
        });
    }
    return static_cast<std::uint8_t>(result);
}

}  // namespace

std::expected<RuntimeConfig, ConfigError> load_runtime_config(
    const std::filesystem::path& path
) {
    LuaStatePtr state(luaL_newstate());
    if (!state) {
        return std::unexpected(ConfigError{
            .message = "failed to create Lua state for runtime config",
        });
    }
    luaL_openlibs(state.get());

    if (luaL_loadbufferx(
            state.get(),
            kLoadConfig.data(),
            kLoadConfig.size(),
            "=[skynet config]",
            "t") != LUA_OK) {
        return std::unexpected(lua_error(state.get(), "failed to load config parser"));
    }

    const std::string path_string = path.string();
    lua_pushlstring(state.get(), path_string.data(), path_string.size());
    if (lua_pcall(state.get(), 1, 1, 0) != LUA_OK) {
        return std::unexpected(lua_error(state.get(), "failed to execute runtime config"));
    }
    if (!lua_istable(state.get(), -1)) {
        return std::unexpected(ConfigError{
            .message = "runtime config did not produce a table",
        });
    }

    RuntimeConfig config;
    lua_pushnil(state.get());
    while (lua_next(state.get(), -2) != 0) {
        if (lua_type(state.get(), -2) != LUA_TSTRING) {
            return std::unexpected(ConfigError{
                .message = "runtime config keys must be strings",
            });
        }
        std::size_t key_size = 0;
        const char* key_data = lua_tolstring(state.get(), -2, &key_size);
        const std::string key(key_data, key_size);
        auto value = scalar_to_string(state.get(), -1, key);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        config.environment.insert_or_assign(key, std::move(*value));
        lua_pop(state.get(), 1);
    }

    if (const auto value = find_config_value(config, "thread")) {
        auto parsed = parse_thread_count(*value);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        config.thread_count = *parsed;
    }
    if (const auto value = find_config_value(config, "harbor")) {
        auto parsed = parse_harbor_id(*value);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        config.harbor_id = *parsed;
    }
    if (const auto value = find_config_value(config, "cpath")) {
        config.module_path = *value;
    }
    if (const auto value = find_config_value(config, "bootstrap")) {
        config.bootstrap = *value;
    }
    if (const auto value = find_config_value(config, "daemon")) {
        config.daemon = std::string(*value);
    }
    if (const auto value = find_config_value(config, "logger")) {
        config.logger = std::string(*value);
    }
    if (const auto value = find_config_value(config, "logservice")) {
        config.log_service = *value;
    }
    if (const auto value = find_config_value(config, "profile")) {
        config.profile = *value == "true";
    }

    config.environment.try_emplace("thread", std::to_string(config.thread_count));
    config.environment.try_emplace("harbor", std::to_string(config.harbor_id));
    config.environment.try_emplace("cpath", config.module_path);
    config.environment.try_emplace("bootstrap", config.bootstrap);
    config.environment.try_emplace("logservice", config.log_service);
    config.environment.try_emplace("profile", config.profile ? "true" : "false");
    return config;
}

std::optional<std::string_view> find_config_value(
    const RuntimeConfig& config,
    std::string_view key
) {
    const auto iterator = config.environment.find(std::string(key));
    if (iterator == config.environment.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

}  // namespace skynet::config
