#include "LogService.hpp"
#include "Runtime.hpp"
#include "LuaService.hpp"
#include "HarborService.hpp"
#include "NativeService.hpp"
#include "runtime_config.hpp"

#include <cstdlib>
#include <csignal>
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

[[nodiscard]] std::filesystem::path resolve_path(
    const std::filesystem::path& config_path,
    std::filesystem::path value
) {
    if (value.is_absolute()) {
        return value;
    }
    return config_path.parent_path() / value;
}

[[nodiscard]] skynet::runtime::LogServiceConfig make_log_config(
    const skynet::config::RuntimeConfig& config,
    const std::filesystem::path& config_path
) {
    std::string path(kDefaultLogFile);
    if (const auto logfile = skynet::config::find_config_value(config, "logfile")) {
        path = *logfile;
    } else if (config.logger && !config.logger->empty()) {
        path = *config.logger;
    }
    return skynet::runtime::LogServiceConfig{
        .file_path = resolve_path(config_path, std::filesystem::path(path)),
    };
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::filesystem::path config_path =
        argc > 1 ? argv[1] : std::string(kDefaultConfig);

    auto loaded_config = skynet::config::load_runtime_config(config_path);
    if (!loaded_config) {
        spdlog::error("failed to load runtime config: {}", loaded_config.error().message);
        return EXIT_FAILURE;
    }

    const auto log_config = make_log_config(*loaded_config, config_path);
    if (loaded_config->harbor_id != 0 || loaded_config->daemon) {
        spdlog::error("stage 3 supports harbor=0 and foreground operation; multi-node/daemon support is pending");
        return EXIT_FAILURE;
    }
    std::signal(SIGPIPE, SIG_IGN);
    skynet::runtime::Runtime runtime(skynet::runtime::RuntimeOptions{
        .worker_count = loaded_config->thread_count,
        .harbor_id = loaded_config->harbor_id,
        .profile = loaded_config->profile,
    });
    for (const auto& [key, value] : loaded_config->environment) {
        (void)runtime.environment().set(key, value);
    }

    const bool registered = runtime.services().register_factory(
        "logger",
        [log_config]() -> skynet::runtime::ServicePtr {
            auto logger = skynet::runtime::LogService::create(log_config);
            return logger ? std::move(*logger) : nullptr;
        });
    if (!registered) {
        spdlog::error("failed to register runtime log service");
        return EXIT_FAILURE;
    }
    (void)runtime.services().register_factory("snlua", [] { return std::make_unique<skynet::service::LuaService>(); });
    (void)runtime.services().register_factory("harbor", [] { return std::make_unique<skynet::service::HarborService>(); });
    runtime.services().set_loader([path = loaded_config->module_path](std::string_view name) {
        return skynet::service::load_native_service(name, path);
    });

    if (auto started = runtime.start(); !started) {
        spdlog::error("failed to start runtime: {}", started.error().message());
        return EXIT_FAILURE;
    }

    auto logger_handle = runtime.launch(
        loaded_config->log_service,
        loaded_config->logger.value_or(std::string{}));
    if (!logger_handle) {
        spdlog::error("failed to launch runtime logger: {}", logger_handle.error().message());
        runtime.request_stop();
        runtime.join();
        return EXIT_FAILURE;
    }
    (void)runtime.name(*logger_handle, "logger");
    const std::string_view bootstrap = loaded_config->bootstrap;
    const auto separator = bootstrap.find_first_of(" \t");
    const auto name = bootstrap.substr(0, separator);
    const auto position = separator == std::string_view::npos ? separator : bootstrap.find_first_not_of(" \t", separator);
    const auto arguments = position == std::string_view::npos ? std::string_view{} : bootstrap.substr(position);
    if (!runtime.launch(name, arguments, true)) {
        runtime.log(0, "Bootstrap launch failed");
        runtime.abort_all();
        runtime.wait();
        return EXIT_FAILURE;
    }
    runtime.wait();
    return runtime.failed() ? EXIT_FAILURE : EXIT_SUCCESS;
}
