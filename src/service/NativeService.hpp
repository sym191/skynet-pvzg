#pragma once
#include "Service.hpp"
#include <string_view>

namespace skynet::service {
runtime::ServicePtr load_native_service(std::string_view name, std::string_view path);
}
