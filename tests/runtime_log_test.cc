#include "LogService.hpp"
#include "runtime_error.hpp"

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

    const std::filesystem::path log_path = temp_dir / "runtime.log";

    auto service = runtime::LogService::create(runtime::LogServiceConfig{
        .file_path = log_path,
    });
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

    auto default_service = runtime::LogService::create();
    if (!default_service) {
        cleanup();
        return fail(default_service.error().message());
    }
    (*default_service)->info("runtime default log file sink smoke");
    (*default_service)->flush();
    if (!std::filesystem::exists("skynet.log")) {
        cleanup();
        return fail("default LogServiceConfig should create skynet.log");
    }
    std::filesystem::remove("skynet.log");

    auto invalid_service = runtime::LogService::create(runtime::LogServiceConfig{
        .file_path = temp_dir,
    });
    if (invalid_service ||
            invalid_service.error() !=
                    runtime::make_error_code(runtime::RuntimeErrc::logger_creation_failed)) {
        cleanup();
        return fail("invalid file sink path should return logger_creation_failed");
    }

    cleanup();
    return EXIT_SUCCESS;
}
