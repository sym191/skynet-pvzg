#pragma once

#include "Service.hpp"

#include <cstddef>
#include <memory>
#include <string_view>

struct lua_State;

namespace skynet::runtime {
class ServiceRegistry;
}

namespace skynet::service {

// LuaService is the C++ runtime wrapper for Skynet's SNLua service. The Lua
// ABI and skynet.core implementation come from the vendored Skynet revision.
class LuaService final : public runtime::Service {
public:
    LuaService();
    ~LuaService() override;

    LuaService(const LuaService&) = delete;
    LuaService& operator=(const LuaService&) = delete;

    bool init(runtime::Context& context, std::string_view args) override;
    void dispatch(runtime::Context& context, runtime::Message message) override;
    void signal(runtime::Context& context, int signal) override;

    [[nodiscard]] lua_State* state() const noexcept;
    [[nodiscard]] std::size_t memory_used() const noexcept;
    [[nodiscard]] std::size_t memory_limit() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool register_lua_service(
    runtime::ServiceRegistry& registry);

}  // namespace skynet::service
