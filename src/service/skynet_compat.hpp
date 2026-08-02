#pragma once

#include <cstddef>
#include <cstdint>

namespace skynet::runtime {
class Context;
}

extern "C" {

struct skynet_context;

using skynet_cb = int (*)(
    skynet_context* context,
    void* user_data,
    int type,
    int session,
    std::uint32_t source,
    const void* message,
    std::size_t size);

}

namespace skynet::service::detail {

class CurrentContextGuard {
public:
    explicit CurrentContextGuard(skynet_context* context) noexcept;
    ~CurrentContextGuard();

    CurrentContextGuard(const CurrentContextGuard&) = delete;
    CurrentContextGuard& operator=(const CurrentContextGuard&) = delete;

private:
    skynet_context* previous_;
};

[[nodiscard]] skynet_context* create_compat_context(
    runtime::Context& context);
void destroy_compat_context(skynet_context* context);

[[nodiscard]] bool dispatch_compat_message(
    skynet_context* context,
    int type,
    int session,
    std::uint32_t source,
    const void* message,
    std::size_t size);

}  // namespace skynet::service::detail
