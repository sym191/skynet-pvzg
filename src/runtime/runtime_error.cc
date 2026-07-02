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
        case RuntimeErrc::invalid_config_boolean:
            return "invalid runtime config boolean";
        case RuntimeErrc::invalid_log_level:
            return "invalid log level";
        case RuntimeErrc::invalid_log_sink:
            return "invalid log sink configuration";
        case RuntimeErrc::logger_creation_failed:
            return "failed to create runtime logger";
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
