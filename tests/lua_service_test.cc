#include "Context.hpp"
#include "LuaService.hpp"
#include "ServiceRegistry.hpp"
#include "lua_api.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using skynet::runtime::Context;
using skynet::runtime::Handle;
using skynet::runtime::Message;
using skynet::runtime::MessageType;
using skynet::service::LuaService;

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

bool set_environment(Context& context, std::string key, std::string value) {
    return context.environment().set(std::move(key), std::move(value));
}

bool configure_lua_paths(Context& context) {
    const std::filesystem::path source_dir(SKPVZG_SOURCE_DIR);
    const std::string upstream_lualib =
        (source_dir / "third_party/skynet/lualib").string();
    return set_environment(
               context,
               "lualoader",
               (source_dir / "third_party/skynet/lualib/loader.lua").string()) &&
        set_environment(
               context,
               "lua_path",
               upstream_lualib + "/?.lua;" + upstream_lualib + "/?/init.lua") &&
        set_environment(
               context,
               "lua_cpath",
               (source_dir / "build/unused/?.so").string()) &&
        set_environment(
               context,
               "luaservice",
               (source_dir / "tests/lua/?.lua").string());
}

std::optional<std::string> global_string(lua_State* state, const char* name) {
    lua_getglobal(state, name);
    if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
    }
    std::size_t size = 0;
    const char* value = lua_tolstring(state, -1, &size);
    std::string result(value, size);
    lua_pop(state, 1);
    return result;
}

std::optional<lua_Integer> global_integer(lua_State* state, const char* name) {
    lua_getglobal(state, name);
    if (!lua_isinteger(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
    }
    const lua_Integer result = lua_tointeger(state, -1);
    lua_pop(state, 1);
    return result;
}

bool call_no_argument_function(
        lua_State* state, const char* name, std::string* error) {
    lua_getglobal(state, name);
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        *error = std::string("missing Lua function: ") + name;
        return false;
    }
    if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
        const char* message = lua_tostring(state, -1);
        *error = message == nullptr ? "unknown Lua error" : message;
        lua_pop(state, 1);
        return false;
    }
    return true;
}

int test_core_compatibility() {
    Context context;
    if (!configure_lua_paths(context) ||
            !set_environment(context, "compat_marker", "available")) {
        return fail("failed to configure Lua service environment");
    }

    std::vector<std::pair<Handle, Message>> sent_messages;
    context.set_send_handler(
        [&sent_messages](Handle destination, Message message) {
            sent_messages.emplace_back(destination, std::move(message));
            return true;
        });

    std::vector<std::string> logs;
    context.set_log_handler(
        [&logs](std::string_view message) { logs.emplace_back(message); });

    LuaService service;
    if (!service.init(context, "core_compat")) {
        return fail(logs.empty() ? "core compatibility service failed to initialize"
                                 : logs.back());
    }
    if (!service.initialized() || service.state() == nullptr ||
            service.memory_used() == 0 ||
            service.memory_limit() != 64U * 1024U * 1024U) {
        return fail("SNLua state did not expose valid lifecycle information");
    }

    Message inbound = Message::text(0x0100'0002U, 77, "incoming");
    service.dispatch(context, std::move(inbound));
    if (global_string(service.state(), "callback_payload") != "incoming" ||
            global_integer(service.state(), "callback_type") != 0 ||
            global_integer(service.state(), "callback_session") != 77 ||
            global_integer(service.state(), "callback_source") != 0x0100'0002U) {
        return fail("skynet.core callback did not preserve message metadata");
    }

    std::string error;
    if (!call_no_argument_function(
            service.state(), "send_compatible_message", &error)) {
        return fail(error);
    }
    if (!lua_isinteger(service.state(), -1)) {
        lua_pop(service.state(), 1);
        return fail("skynet.core.send did not return an allocated session");
    }
    const int allocated_session =
        static_cast<int>(lua_tointeger(service.state(), -1));
    lua_pop(service.state(), 1);
    if (allocated_session <= 0 || sent_messages.size() != 1 ||
            sent_messages.front().first != 0x0100'0002U ||
            sent_messages.front().second.source != context.handle() ||
            sent_messages.front().second.type != MessageType::lua ||
            sent_messages.front().second.session != allocated_session ||
            sent_messages.front().second.text_payload() != "outbound") {
        return fail("skynet.core.send did not preserve Skynet send semantics");
    }

    service.signal(context, 1);
    bool saw_memory_report = false;
    for (const std::string& log : logs) {
        if (log.find("Current Memory") != std::string::npos) {
            saw_memory_report = true;
            break;
        }
    }
    if (!saw_memory_report) {
        return fail("SNLua signal did not report memory usage");
    }
    return EXIT_SUCCESS;
}

int test_standard_skynet_service() {
    Context context;
    if (!configure_lua_paths(context)) {
        return fail("failed to configure standard Skynet service paths");
    }

    std::vector<std::string> logs;
    context.set_log_handler(
        [&logs](std::string_view message) { logs.emplace_back(message); });

    LuaService service;
    if (!service.init(context, "standard_service")) {
        return fail(logs.empty() ? "standard Skynet service failed to initialize"
                                 : logs.back());
    }

    auto startup_message = context.message_queue().pop();
    if (!startup_message || startup_message->type != MessageType::response) {
        return fail("skynet.timeout(0) did not schedule the startup response");
    }
    service.dispatch(context, std::move(*startup_message));

    bool saw_startup_log = false;
    for (const std::string& log : logs) {
        if (log.find("standard Skynet service started") != std::string::npos) {
            saw_startup_log = true;
            break;
        }
    }
    if (!saw_startup_log || !context.exit_requested()) {
        return fail("unmodified skynet.lua service lifecycle did not complete");
    }
    return EXIT_SUCCESS;
}

int test_initialization_failure() {
    Context context;
    if (!set_environment(context, "lualoader", "/missing/skynet/loader.lua")) {
        return fail("failed to configure invalid loader path");
    }
    std::vector<std::string> logs;
    context.set_log_handler(
        [&logs](std::string_view message) { logs.emplace_back(message); });

    LuaService service;
    if (service.init(context, "missing_service") || service.initialized() ||
            !context.exit_requested()) {
        return fail("LuaService did not fail closed for a missing loader");
    }
    if (logs.empty() || logs.front().find("Can't load") == std::string::npos) {
        return fail("LuaService loader failure did not preserve diagnostics");
    }
    return EXIT_SUCCESS;
}

int test_service_registration() {
    skynet::runtime::ServiceRegistry registry;
    if (!skynet::service::register_lua_service(registry) ||
            !registry.contains("snlua")) {
        return fail("LuaService did not register under the Skynet snlua name");
    }
    auto service = registry.create("snlua");
    if (dynamic_cast<LuaService*>(service.get()) == nullptr) {
        return fail("snlua factory did not create a LuaService");
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (const int result = test_core_compatibility(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_standard_skynet_service();
            result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_initialization_failure();
            result != EXIT_SUCCESS) {
        return result;
    }
    return test_service_registration();
}
