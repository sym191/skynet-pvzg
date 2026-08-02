#include "skynet_compat.hpp"

#include "Context.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

struct skynet_context {
    skynet::runtime::Context* runtime_context = nullptr;
    void* callback_user_data = nullptr;
    skynet_cb callback = nullptr;
    std::string command_result;
};

namespace {

constexpr int kTypeMask = 0xff;
constexpr int kTagDontCopy = 0x1'0000;
constexpr int kTagAllocateSession = 0x2'0000;

const auto kClockStart = std::chrono::steady_clock::now();
thread_local skynet_context* current_context = nullptr;

[[nodiscard]] std::span<const std::byte> as_bytes(
        const void* message, std::size_t size) {
    if (message == nullptr || size == 0) {
        return {};
    }
    return {static_cast<const std::byte*>(message), size};
}

void release_owned_message(int type, void* message) {
    if ((type & kTagDontCopy) != 0) {
        std::free(message);
    }
}

}  // namespace

namespace skynet::service::detail {

CurrentContextGuard::CurrentContextGuard(skynet_context* context) noexcept
        : previous_(current_context) {
    current_context = context;
}

CurrentContextGuard::~CurrentContextGuard() {
    current_context = previous_;
}

skynet_context* create_compat_context(runtime::Context& context) {
    auto compat_context = std::make_unique<skynet_context>();
    compat_context->runtime_context = &context;
    return compat_context.release();
}

void destroy_compat_context(skynet_context* context) {
    delete context;
}

bool dispatch_compat_message(
        skynet_context* context,
        int type,
        int session,
        std::uint32_t source,
        const void* message,
        std::size_t size) {
    if (context == nullptr || context->runtime_context == nullptr ||
            context->callback == nullptr) {
        return false;
    }
    CurrentContextGuard guard(context);
    context->runtime_context->record_dispatch();
    (void)context->callback(
        context,
        context->callback_user_data,
        type,
        session,
        source,
        message,
        size);
    return true;
}

}  // namespace skynet::service::detail

extern "C" {

void skynet_error(skynet_context* context, const char* format, ...) {
    if (context == nullptr || context->runtime_context == nullptr ||
            format == nullptr) {
        return;
    }

    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    const int required = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);

    if (required < 0) {
        va_end(arguments);
        return;
    }
    std::string message(static_cast<std::size_t>(required), '\0');
    std::vsnprintf(message.data(), message.size() + 1, format, arguments);
    va_end(arguments);
    context->runtime_context->log(message);
}

const char* skynet_command(
        skynet_context* context, const char* command, const char* parameter) {
    if (context == nullptr || context->runtime_context == nullptr ||
            command == nullptr) {
        return nullptr;
    }
    skynet::service::detail::CurrentContextGuard guard(context);
    auto result = context->runtime_context->command(
        command,
        parameter == nullptr
            ? std::nullopt
            : std::optional<std::string_view>(parameter));
    if (!result) {
        return nullptr;
    }
    context->command_result = std::move(*result);
    return context->command_result.c_str();
}

std::uint32_t skynet_queryname(skynet_context* context, const char* name) {
    if (context == nullptr || context->runtime_context == nullptr ||
            name == nullptr) {
        return 0;
    }
    return context->runtime_context->resolve(name);
}

int skynet_send(
        skynet_context* context,
        std::uint32_t source,
        std::uint32_t destination,
        int type,
        int session,
        void* message,
        std::size_t size) {
    if (context == nullptr || context->runtime_context == nullptr) {
        release_owned_message(type, message);
        return -1;
    }
    skynet::service::detail::CurrentContextGuard guard(context);
    const int result = context->runtime_context->send(
        destination,
        source,
        type & kTypeMask,
        session,
        as_bytes(message, size),
        (type & kTagAllocateSession) != 0);
    release_owned_message(type, message);
    return result;
}

int skynet_sendname(
        skynet_context* context,
        std::uint32_t source,
        const char* destination,
        int type,
        int session,
        void* message,
        std::size_t size) {
    if (context == nullptr || context->runtime_context == nullptr ||
            destination == nullptr) {
        release_owned_message(type, message);
        return -1;
    }
    skynet::service::detail::CurrentContextGuard guard(context);
    const int result = context->runtime_context->send(
        destination,
        source,
        type & kTypeMask,
        session,
        as_bytes(message, size),
        (type & kTagAllocateSession) != 0);
    release_owned_message(type, message);
    return result;
}

int skynet_isremote(skynet_context* context, std::uint32_t handle, int* harbor) {
    if (context == nullptr || context->runtime_context == nullptr) {
        if (harbor != nullptr) {
            *harbor = 0;
        }
        return 0;
    }
    return context->runtime_context->is_remote(handle, harbor) ? 1 : 0;
}

void skynet_callback(skynet_context* context, void* user_data, skynet_cb callback) {
    if (context == nullptr) {
        return;
    }
    context->callback_user_data = user_data;
    context->callback = callback;
}

std::uint32_t skynet_current_handle(void) {
    return current_context == nullptr || current_context->runtime_context == nullptr
        ? 0
        : current_context->runtime_context->handle();
}

std::uint64_t skynet_now(void) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - kClockStart).count() /
        10);
}

void skynet_debug_memory(const char* information) {
    if (current_context != nullptr && current_context->runtime_context != nullptr) {
        current_context->runtime_context->log(
            information == nullptr ? "" : information);
    }
}

void* skynet_lalloc(void* pointer, std::size_t old_size, std::size_t new_size) {
    (void)old_size;
    if (new_size == 0) {
        std::free(pointer);
        return nullptr;
    }
    return std::realloc(pointer, new_size);
}

}  // extern "C"
