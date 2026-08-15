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

// LuaService provides the SNLua lifecycle expected by the original Lua layer.
// Its native runtime and skynet.core implementation are written in C++.
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
