#include "runtime_config.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

std::filesystem::path make_temp_directory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("skpvzg_config_test_" + std::to_string(stamp));
}

}  // namespace

int main() {
    auto generated = skynet::config::load_runtime_config(
        std::filesystem::path(SKPVZG_REFACTOR_CONFIG));
    if (!generated) {
        return fail(generated.error().message);
    }
    if (generated->thread_count != 8 || generated->log_service != "logger") {
        return fail("generated config did not preserve runtime defaults");
    }
    const auto lua_service = skynet::config::find_config_value(*generated, "luaservice");
    if (!lua_service || lua_service->find("/lua/service/?.lua") == std::string_view::npos) {
        return fail("Lua expressions were not evaluated in generated config");
    }

    const std::filesystem::path temp_dir = make_temp_directory();
    std::filesystem::create_directories(temp_dir);
    const auto cleanup = [&temp_dir] {
        std::error_code error;
        std::filesystem::remove_all(temp_dir, error);
    };

    { std::ofstream empty(temp_dir / "empty.lua"); }
    auto defaults = skynet::config::load_runtime_config(temp_dir / "empty.lua");
    if (!defaults || defaults->environment.at("harbor") != "1" ||
        defaults->environment.at("thread") != "8" || defaults->environment.at("profile") != "true" ||
        defaults->environment.at("bootstrap") != "snlua bootstrap" ||
        defaults->environment.at("cpath") != "./cservice/?.so" ||
        defaults->environment.at("logservice") != "logger" || defaults->environment.contains("daemon") ||
        defaults->environment.contains("logger")) {
        cleanup();
        return fail("effective defaults must also be visible through GETENV");
    }

    {
        std::ofstream child(temp_dir / "child.lua");
        child << "thread = 3\nharbor = 7\nprofile = false\n";
        std::ofstream root(temp_dir / "root.lua");
        root << "include 'child.lua'\n"
             << "root = 'prefix'\n"
             << "bootstrap = root .. ' bootstrap'\n";
    }
    auto included = skynet::config::load_runtime_config(temp_dir / "root.lua");
    if (!included || included->thread_count != 3 || included->harbor_id != 7 ||
        included->profile || included->bootstrap != "prefix bootstrap") {
        cleanup();
        return fail(included ? "included config values were incorrect" :
            included.error().message);
    }

    {
        std::ofstream invalid(temp_dir / "invalid.lua");
        invalid << "thread = 0\n";
    }
    if (skynet::config::load_runtime_config(temp_dir / "invalid.lua")) {
        cleanup();
        return fail("invalid thread count should be rejected");
    }

    cleanup();
    return EXIT_SUCCESS;
}
