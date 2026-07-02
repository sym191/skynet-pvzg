#include "LogService.hpp"
#include "runtime_error.hpp"

#include <cctype>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/io_context.hpp>
#include <spdlog/spdlog.h>

namespace {

constexpr std::string_view kDefaultConfig = "configs/skynet_refactor.lua";
constexpr std::string_view kDefaultLogFile = "skynet.log";

[[nodiscard]] std::string trim(std::string_view value) {
    auto begin = value.begin();
    auto end = value.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    while (begin != end &&
            std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

[[nodiscard]] std::expected<std::optional<std::string>, std::error_code>
parse_optional_string(std::string_view raw_value) {
    const std::string value = trim(raw_value);
    if (value.empty() || value == "nil") {
        return std::optional<std::string>{};
    }
    if (value.front() != '"' && value.front() != '\'') {
        return value;
    }

    const char quote = value.front();
    const std::size_t close_pos = value.find(quote, 1);
    if (close_pos == std::string::npos) {
        return std::unexpected(
            skynet::runtime::make_error_code(
                skynet::runtime::RuntimeErrc::invalid_config_string));
    }
    return value.substr(1, close_pos - 1);
}

[[nodiscard]] std::filesystem::path resolve_config_path(
        const std::filesystem::path& startup_config_path,
        std::filesystem::path configured_path) {
    if (configured_path.is_absolute()) {
        return configured_path;
    }
    return startup_config_path.parent_path() / configured_path;
}

[[nodiscard]] std::expected<skynet::runtime::LogServiceConfig, std::error_code>
load_log_service_config(const std::filesystem::path& startup_config_path) {
    skynet::runtime::LogServiceConfig config{
        .file_path = resolve_config_path(
            startup_config_path,
            std::filesystem::path{kDefaultLogFile}),
    };

    std::ifstream input(startup_config_path);
    if (!input) {
        return std::unexpected(
            skynet::runtime::make_error_code(
                skynet::runtime::RuntimeErrc::config_open_failed));
    }

    std::string line;
    while (std::getline(input, line)) {
        const std::string clean_line = trim(line);
        const std::size_t equal_pos = clean_line.find('=');
        if (equal_pos == std::string::npos) {
            continue;
        }
        if (trim(std::string_view(clean_line).substr(0, equal_pos)) != "logfile") {
            continue;
        }

        auto file_path = parse_optional_string(
            std::string_view(clean_line).substr(equal_pos + 1));
        if (!file_path) {
            return std::unexpected(file_path.error());
        }
        if (*file_path && !(**file_path).empty()) {
            config.file_path = resolve_config_path(
                startup_config_path,
                std::filesystem::path{**file_path});
        }
    }

    return config;
}

}  // namespace 

int main(int argc, char* argv[]) {
    const std::string_view config_path = argc > 1 ? argv[1] : kDefaultConfig;

    auto log_config = load_log_service_config(std::filesystem::path{config_path});
    if (!log_config) {
        spdlog::error(
            "failed to load log config: {}",
            log_config.error().message()
        );
        return EXIT_FAILURE;
    }

    auto log_service = skynet::runtime::LogService::create(std::move(*log_config));
    if (!log_service) {
        spdlog::error(
            "failed to initialize log service: {}",
            log_service.error().message()
        );
        return EXIT_FAILURE;
    }

    asio::io_context io_context;
    (*log_service)->info("skynet-pvzg refactor runtime scaffold");
    (*log_service)->info(std::string("config: ") + std::string(config_path));
    (*log_service)->info(
        std::string("asio stopped: ") +
        (io_context.stopped() ? "true" : "false"));
    (*log_service)->flush();

    return EXIT_SUCCESS;
}
