#include <cstdlib>
#include <string_view>

#include <asio/io_context.hpp>
#include <spdlog/spdlog.h>

namespace {

constexpr std::string_view kDefaultConfig = "configs/skynet_refactor.lua";

}  // namespace 

int main(int argc, char* argv[]) {
    const std::string_view config_path = argc > 1 ? argv[1] : kDefaultConfig;

    asio::io_context io_context;
    spdlog::info("skynet-pvzg refactor runtime scaffold");
    spdlog::info("config: {}", config_path);
    spdlog::info("asio stopped: {}", io_context.stopped());

    return EXIT_SUCCESS;
}
