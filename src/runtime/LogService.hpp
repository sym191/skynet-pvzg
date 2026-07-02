#pragma once

#include "Service.hpp"
#include "runtime_error.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <spdlog/common.h>
#include <spdlog/logger.h>

namespace skynet::runtime {

struct LogConfig {
    bool console_enabled = true;
    std::optional<std::filesystem::path> file_path;
    spdlog::level::level_enum level = spdlog::level::info;
    std::string pattern = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v";
};

[[nodiscard]] std::expected<LogConfig, std::error_code> load_log_config(
        const std::filesystem::path& startup_config_path);

[[nodiscard]] std::expected<std::shared_ptr<spdlog::logger>, std::error_code>
create_runtime_logger(const LogConfig& config);

class LogService final : public Service {
public:
    explicit LogService(std::shared_ptr<spdlog::logger> logger);

    [[nodiscard]] static std::expected<std::unique_ptr<LogService>, std::error_code>
    create_from_startup_config(
            const std::filesystem::path& startup_config_path);

    bool init(Context& context, std::string_view args) override;
    void dispatch(Context& context, Message msg) override;
    void signal(Context& context, int signal) override;

    void log(spdlog::level::level_enum level, std::string_view message);
    void info(std::string_view message);
    void flush();

    [[nodiscard]] std::shared_ptr<spdlog::logger> logger() const noexcept;

private:
    std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace skynet::runtime
