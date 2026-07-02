#pragma once

#include <system_error>
#include <type_traits>

namespace skynet::runtime {

enum class RuntimeErrc {
    success = 0,
    config_open_failed,
    invalid_config_string,
    invalid_config_boolean,
    invalid_log_level,
    invalid_log_sink,
    logger_creation_failed,
};

[[nodiscard]] const std::error_category& runtime_error_category() noexcept;

[[nodiscard]] std::error_code make_error_code(RuntimeErrc error) noexcept;

}  // namespace skynet::runtime

namespace std {

template <>
struct is_error_code_enum<skynet::runtime::RuntimeErrc> : true_type {};

}  // namespace std
