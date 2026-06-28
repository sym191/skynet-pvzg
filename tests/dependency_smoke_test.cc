#include <cstdlib>
#include <memory>

#include <asio/io_context.hpp>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

int main() {
    asio::io_context io_context;
    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    spdlog::logger logger("dependency_smoke", sink);
    logger.info("asio stopped: {}", io_context.stopped());
    return EXIT_SUCCESS;
}
