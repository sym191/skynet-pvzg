#include "LogService.hpp"

#include "runtime_error.hpp"

#include <exception>
#include <utility>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace skynet::runtime {
namespace {

constexpr spdlog::level::level_enum kDefaultLevel = spdlog::level::info;
constexpr std::string_view kDefaultPattern =
    "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v";

}  // namespace

std::expected<std::unique_ptr<LogService>, std::error_code> LogService::create(
        LogServiceConfig config) {
    try {
        if (config.file_path.empty()) {
            config.file_path = "skynet.log";
        }

        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                config.file_path.string(), true));

        auto logger = std::make_shared<spdlog::logger>(
                "skynet", sinks.begin(), sinks.end());
        logger->set_level(kDefaultLevel);
        logger->set_pattern(std::string(kDefaultPattern));
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(logger);
        return std::make_unique<LogService>(std::move(logger));
    } catch (const std::exception& error) {
        (void)error;
        return std::unexpected(make_error_code(RuntimeErrc::logger_creation_failed));
    }
}

LogService::LogService(std::shared_ptr<spdlog::logger> logger)
        : logger_(std::move(logger)) {}

bool LogService::init(Context& context, std::string_view args) {
    (void)context;
    if (!args.empty()) {
        info(args);
    }
    return logger_ != nullptr;
}

void LogService::dispatch(Context& context, Message msg) {
    (void)context;
    if (!logger_) {
        return;
    }
    logger_->info("[source={:08x} session={}] {}", msg.source, msg.session,
        msg.StringPayload());
}

void LogService::signal(Context& context, int signal) {
    (void)context;
    if (logger_) {
        logger_->info("log service received signal {}", signal);
        logger_->flush();
    }
}

void LogService::log(spdlog::level::level_enum level, std::string_view message) {
    if (!logger_) {
        return;
    }
    logger_->log(level, "{}", message);
}

void LogService::info(std::string_view message) {
    log(spdlog::level::info, message);
}

void LogService::flush() {
    if (logger_) {
        logger_->flush();
    }
}

std::shared_ptr<spdlog::logger> LogService::logger() const noexcept {
    return logger_;
}

}  // namespace skynet::runtime
