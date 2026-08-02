#include "LuaService.hpp"

#include "Context.hpp"
#include "ServiceRegistry.hpp"
#include "skynet_compat.hpp"
#include "skynet_snlua_bridge.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace skynet::service {

struct LuaService::Impl {
    Impl()
            : service(skynet_snlua_create()) {}

    ~Impl() {
        skynet_snlua_release(service);
        detail::destroy_compat_context(compat_context);
    }

    snlua* service = nullptr;
    skynet_context* compat_context = nullptr;
    runtime::Context* runtime_context = nullptr;
    bool initialized = false;
};

LuaService::LuaService()
        : impl_(std::make_unique<Impl>()) {}

LuaService::~LuaService() = default;

bool LuaService::init(runtime::Context& context, std::string_view args) {
    if (impl_->initialized || impl_->service == nullptr) {
        return false;
    }

    impl_->compat_context = detail::create_compat_context(context);
    if (impl_->compat_context == nullptr) {
        return false;
    }
    impl_->runtime_context = &context;

    const std::string arguments(args);
    detail::CurrentContextGuard current_context_guard(impl_->compat_context);
    if (skynet_snlua_init_direct(
            impl_->service,
            impl_->compat_context,
            arguments.c_str(),
            arguments.size()) != 0) {
        (void)context.command("EXIT");
        return false;
    }

    impl_->initialized = true;
    return true;
}

void LuaService::dispatch(runtime::Context& context, runtime::Message message) {
    if (!impl_->initialized || impl_->runtime_context != &context) {
        context.log("LuaService received a message before successful initialization");
        return;
    }

    const bool dispatched = detail::dispatch_compat_message(
        impl_->compat_context,
        static_cast<int>(message.type),
        message.session,
        message.source,
        message.data.empty() ? nullptr : message.data.data(),
        message.data.size());
    if (!dispatched) {
        context.log("LuaService has no registered message callback");
    }
}

void LuaService::signal(runtime::Context& context, int signal_value) {
    if (!impl_->initialized || impl_->runtime_context != &context) {
        return;
    }
    skynet_snlua_signal(impl_->service, signal_value);
}

lua_State* LuaService::state() const noexcept {
    return skynet_snlua_state(impl_->service);
}

std::size_t LuaService::memory_used() const noexcept {
    return skynet_snlua_memory(impl_->service);
}

std::size_t LuaService::memory_limit() const noexcept {
    return skynet_snlua_memory_limit(impl_->service);
}

bool LuaService::initialized() const noexcept {
    return impl_->initialized;
}

bool register_lua_service(runtime::ServiceRegistry& registry) {
    return registry.register_factory(
        "snlua", [] { return std::make_unique<LuaService>(); });
}

}  // namespace skynet::service
