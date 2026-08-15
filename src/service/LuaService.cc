#include "LuaService.hpp"

#include "Context.hpp"
#include "LuaRuntime.hpp"
#include "ServiceRegistry.hpp"

#include <cstddef>
#include <memory>
#include <utility>

namespace skynet::service {

struct LuaService::Impl {
    detail::LuaRuntime lua_runtime;
    runtime::Context* runtime_context = nullptr;
};

LuaService::LuaService()
        : impl_(std::make_unique<Impl>()) {}

LuaService::~LuaService() = default;

bool LuaService::init(runtime::Context& context, std::string_view args) {
    if (impl_->runtime_context != nullptr) {
        return false;
    }
    impl_->runtime_context = &context;
    if (!impl_->lua_runtime.Init(context, args)) {
        return false;
    }
    return true;
}

void LuaService::dispatch(runtime::Context& context, runtime::Message message) {
    if (!impl_->lua_runtime.initialized() || impl_->runtime_context != &context) {
        context.log("LuaService received a message before successful initialization");
        return;
    }
    if (!impl_->lua_runtime.Dispatch(std::move(message))) {
        context.log("LuaService has no registered message callback");
    }
}

void LuaService::signal(runtime::Context& context, int signal_value) {
    if (!impl_->lua_runtime.initialized() || impl_->runtime_context != &context) {
        return;
    }
    impl_->lua_runtime.Signal(signal_value);
}

lua_State* LuaService::state() const noexcept {
    return impl_->lua_runtime.state();
}

std::size_t LuaService::memory_used() const noexcept {
    return impl_->lua_runtime.memory_used();
}

std::size_t LuaService::memory_limit() const noexcept {
    return impl_->lua_runtime.memory_limit();
}

bool LuaService::initialized() const noexcept {
    return impl_->lua_runtime.initialized();
}

bool register_lua_service(runtime::ServiceRegistry& registry) {
    return registry.register_factory(
        "snlua", [] { return std::make_unique<LuaService>(); });
}

}  // namespace skynet::service
