#include "LogService.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

std::filesystem::path make_temp_dir() {
    const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("skpvzg_runtime_log_test_" + std::to_string(stamp));
}

}  // namespace

int main() {
    namespace runtime = skynet::runtime;

    const std::filesystem::path temp_dir = make_temp_dir();
    std::filesystem::create_directories(temp_dir);
    const auto cleanup = [&temp_dir] {
        std::error_code error;
        std::filesystem::remove_all(temp_dir, error);
    };

    const std::filesystem::path config_path = temp_dir / "skynet.lua";
    const std::filesystem::path log_path = temp_dir / "runtime.log";

    {
        std::ofstream config(config_path);
        config << "logger = \"stdout\"\n"
            << "log_console = false\n"
            << "logfile = \"runtime.log\"\n"
            << "log_level = \"debug\"\n";
    }

    auto config = runtime::load_log_config(config_path);
    if (!config) {
        cleanup();
        return fail(config.error().message());
    }
    if (config->console_enabled) {
        cleanup();
        return fail("log_console=false should disable the console sink");
    }
    if (!config->file_path || *config->file_path != log_path) {
        cleanup();
        return fail("logfile should resolve relative to the startup config");
    }
    if (config->level != spdlog::level::debug) {
        cleanup();
        return fail("log_level should configure the spdlog level");
    }

    auto service = runtime::LogService::create_from_startup_config(config_path);
    if (!service) {
        cleanup();
        return fail(service.error().message());
    }
    (*service)->info("runtime log file sink smoke");
    (*service)->flush();

    std::ifstream log_file(log_path);
    std::string content(
            (std::istreambuf_iterator<char>(log_file)),
            std::istreambuf_iterator<char>());
    if (content.find("runtime log file sink smoke") == std::string::npos) {
        cleanup();
        return fail("log file did not receive the runtime log message");
    }

    {
        std::ofstream config_with_logger_file(config_path);
        config_with_logger_file << "log_console = false\n"
            << "logger = \"logger-field.log\"\n";
    }
    auto logger_file_config = runtime::load_log_config(config_path);
    if (!logger_file_config) {
        cleanup();
        return fail(logger_file_config.error().message());
    }
    if (!logger_file_config->file_path ||
            logger_file_config->file_path->filename() != "logger-field.log") {
        cleanup();
        return fail("logger should act as a file sink path when it is not stdout");
    }

    auto missing_config = runtime::load_log_config(temp_dir / "missing.lua");
    if (missing_config ||
            missing_config.error() !=
                    runtime::make_error_code(runtime::RuntimeErrc::config_open_failed)) {
        cleanup();
        return fail("missing config should return config_open_failed");
    }

    {
        std::ofstream config_with_bad_level(config_path);
        config_with_bad_level << "logger = \"stdout\"\n"
            << "log_level = \"verbose\"\n";
    }
    auto bad_level_config = runtime::load_log_config(config_path);
    if (bad_level_config ||
            bad_level_config.error() !=
                    runtime::make_error_code(runtime::RuntimeErrc::invalid_log_level)) {
        cleanup();
        return fail("bad log_level should return invalid_log_level");
    }

    cleanup();
    return EXIT_SUCCESS;
}
