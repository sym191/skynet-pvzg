#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace skynet::config {

struct RuntimeConfig {
    std::size_t thread_count{8};
    std::uint8_t harbor_id{1};
    std::string module_path{"./cservice/?.so"};
    std::string bootstrap{"snlua bootstrap"};
    std::optional<std::string> daemon;
    std::optional<std::string> logger;
    std::string log_service{"logger"};
    bool profile{true};
    std::unordered_map<std::string, std::string> environment;
};

struct ConfigError {
    std::string message;
};

[[nodiscard]] std::expected<RuntimeConfig, ConfigError> load_runtime_config(
    const std::filesystem::path& path
);

[[nodiscard]] std::optional<std::string_view> find_config_value(
    const RuntimeConfig& config,
    std::string_view key
);

}  // namespace skynet::config
