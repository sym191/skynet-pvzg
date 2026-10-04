#include "runtime_config.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int Fail(std::string_view message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

std::filesystem::path MakeTempDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("skpvzg_config_test_" + std::to_string(stamp));
}

}  // namespace

int main() {
    auto generated = skynet::config::LoadRuntimeConfig(
        std::filesystem::path(SKPVZG_REFACTOR_CONFIG));
    if (!generated) {
        return Fail(generated.error().message);
    }
    if (generated->thread_count != 8 || generated->log_service != "logger") {
        return Fail("generated config did not preserve runtime defaults");
    }
    const auto lua_service = skynet::config::FindConfigValue(*generated, "luaservice");
    if (!lua_service || lua_service->find("/lua/service/?.lua") == std::string_view::npos) {
        return Fail("Lua expressions were not evaluated in generated config");
    }

    const std::filesystem::path temp_dir = MakeTempDirectory();
    std::filesystem::create_directories(temp_dir);
    const auto cleanup = [&temp_dir] {
        std::error_code error;
        std::filesystem::remove_all(temp_dir, error);
    };

    {
        std::ofstream child(temp_dir / "child.lua");
        child << "thread = 3\nharbor = 7\nprofile = false\n";
        std::ofstream root(temp_dir / "root.lua");
        root << "include 'child.lua'\n"
             << "root = 'prefix'\n"
             << "bootstrap = root .. ' bootstrap'\n";
    }
    auto included = skynet::config::LoadRuntimeConfig(temp_dir / "root.lua");
    if (!included || included->thread_count != 3 || included->harbor_id != 7 ||
        included->profile || included->bootstrap != "prefix bootstrap") {
        cleanup();
        return Fail(included ? "included config values were incorrect" :
            included.error().message);
    }

    {
        std::ofstream invalid(temp_dir / "invalid.lua");
        invalid << "thread = 0\n";
    }
    if (skynet::config::LoadRuntimeConfig(temp_dir / "invalid.lua")) {
        cleanup();
        return Fail("invalid thread count should be rejected");
    }

    cleanup();
    return EXIT_SUCCESS;
}
