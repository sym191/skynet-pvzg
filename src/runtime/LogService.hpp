#pragma once

#include "Service.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <string_view>
#include <system_error>

#include <spdlog/common.h>
#include <spdlog/logger.h>

namespace skynet::runtime {

struct LogServiceConfig {
    std::filesystem::path file_path{"skynet.log"};
};

class LogService final : public Service {
public:
    explicit LogService(std::shared_ptr<spdlog::logger> logger);
    ~LogService() override;

    [[nodiscard]] static std::expected<std::unique_ptr<LogService>, std::error_code>
    create(LogServiceConfig config = {});

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
