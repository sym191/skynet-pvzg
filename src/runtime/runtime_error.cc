#include "runtime_error.hpp"

#include <string>
#include <system_error>

namespace skynet::runtime {
namespace {

class RuntimeErrorCategory final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override {
        return "skynet.runtime";
    }

    [[nodiscard]] std::string message(int value) const override {
        switch (static_cast<RuntimeErrc>(value)) {
        case RuntimeErrc::success:
            return "success";
        case RuntimeErrc::config_open_failed:
            return "failed to open runtime config";
        case RuntimeErrc::invalid_config_string:
            return "invalid runtime config string";
        case RuntimeErrc::logger_creation_failed:
            return "failed to create runtime logger";
        case RuntimeErrc::invalid_argument:
            return "invalid runtime argument";
        case RuntimeErrc::runtime_not_running:
            return "runtime is not running";
        case RuntimeErrc::runtime_stopping:
            return "runtime is stopping";
        case RuntimeErrc::scheduler_start_failed:
            return "failed to start runtime scheduler";
        case RuntimeErrc::timer_start_failed:
            return "failed to start runtime timer";
        case RuntimeErrc::service_not_found:
            return "service factory was not found";
        case RuntimeErrc::service_init_failed:
            return "service initialization failed";
        case RuntimeErrc::handle_exhausted:
            return "runtime handle space is exhausted";
        }

        return "unknown runtime error";
    }
};

}  // namespace

const std::error_category& runtime_error_category() noexcept {
    static const RuntimeErrorCategory category;
    return category;
}

std::error_code make_error_code(RuntimeErrc error) noexcept {
    return {
        static_cast<int>(error),
        runtime_error_category()
    };
}

}  // namespace skynet::runtime
