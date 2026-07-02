#include "LogService.hpp"

#include <cstdlib>
#include <string_view>

#include <asio/io_context.hpp>
#include <spdlog/spdlog.h>

namespace {

constexpr std::string_view kDefaultConfig = "configs/skynet_refactor.lua";

}  // namespace 

int main(int argc, char* argv[]) {
    const std::string_view config_path = argc > 1 ? argv[1] : kDefaultConfig;

    auto log_service =
        skynet::runtime::LogService::create_from_startup_config(config_path);
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
