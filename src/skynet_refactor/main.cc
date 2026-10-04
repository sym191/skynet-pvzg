#include "LogService.hpp"
#include "Runtime.hpp"
#include "runtime_config.hpp"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

namespace {

constexpr std::string_view kDefaultConfig = "configs/skynet_refactor.lua";
constexpr std::string_view kDefaultLogFile = "skynet.log";

[[nodiscard]] std::filesystem::path ResolvePath(
    const std::filesystem::path& config_path,
    std::filesystem::path value
) {
    if (value.is_absolute()) {
        return value;
    }
    return config_path.parent_path() / value;
}

[[nodiscard]] skynet::runtime::LogServiceConfig MakeLogConfig(
    const skynet::config::RuntimeConfig& config,
    const std::filesystem::path& config_path
) {
    std::string path(kDefaultLogFile);
    if (const auto logfile = skynet::config::FindConfigValue(config, "logfile")) {
        path = *logfile;
    } else if (config.logger && !config.logger->empty()) {
        path = *config.logger;
    }
    return skynet::runtime::LogServiceConfig{
        .file_path = ResolvePath(config_path, std::filesystem::path(path)),
    };
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::filesystem::path config_path =
        argc > 1 ? argv[1] : std::string(kDefaultConfig);

    auto loaded_config = skynet::config::LoadRuntimeConfig(config_path);
    if (!loaded_config) {
        spdlog::error("failed to load runtime config: {}", loaded_config.error().message);
        return EXIT_FAILURE;
    }

    const auto log_config = MakeLogConfig(*loaded_config, config_path);
    skynet::runtime::Runtime runtime(skynet::runtime::RuntimeOptions{
        .worker_count = loaded_config->thread_count,
        .harbor_id = loaded_config->harbor_id,
        .profile = loaded_config->profile,
    });
    for (const auto& [key, value] : loaded_config->environment) {
        (void)runtime.environment().set(key, value);
    }

    const bool registered = runtime.services().register_factory(
        loaded_config->log_service,
        [log_config]() -> skynet::runtime::ServicePtr {
            auto logger = skynet::runtime::LogService::create(log_config);
            return logger ? std::move(*logger) : nullptr;
        });
    if (!registered) {
        spdlog::error("failed to register runtime log service");
        return EXIT_FAILURE;
    }

    if (auto started = runtime.Start(); !started) {
        spdlog::error("failed to start runtime: {}", started.error().message());
        return EXIT_FAILURE;
    }

    auto logger_handle = runtime.Launch(
        loaded_config->log_service,
        loaded_config->logger.value_or(std::string{}));
    if (!logger_handle) {
        spdlog::error("failed to launch runtime logger: {}", logger_handle.error().message());
        runtime.RequestStop();
        runtime.Join();
        return EXIT_FAILURE;
    }
    (void)runtime.Name(*logger_handle, "logger");
    runtime.Log(*logger_handle, "skynet-pvzg runtime stage 1-2 initialized");
    runtime.Log(*logger_handle, "Lua bootstrap is deferred to stage 3");

    runtime.RequestStop();
    runtime.Join();
    return EXIT_SUCCESS;
}
