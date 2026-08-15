#ifndef SKYNET_PVZG_SERVICE_LUA_RUNTIME_HPP_
#define SKYNET_PVZG_SERVICE_LUA_RUNTIME_HPP_

#include "Message.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <string_view>

struct lua_State;

namespace skynet::runtime {
class Context;
}

namespace skynet::service::detail {

// LuaRuntime owns one Lua VM and is the native boundary consumed by
// skynet.core. It intentionally exposes no Skynet C runtime ABI.
class LuaRuntime {
public:
    LuaRuntime();
    ~LuaRuntime();

    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    [[nodiscard]] bool Init(runtime::Context& context, std::string_view args);
    [[nodiscard]] bool Dispatch(runtime::Message message);
    void Signal(int signal);

    [[nodiscard]] lua_State* state() const noexcept;
    [[nodiscard]] std::size_t memory_used() const noexcept;
    [[nodiscard]] std::size_t memory_limit() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

    [[nodiscard]] runtime::Context& context() const;
    void SetCallback(lua_State* state, int function_index, bool forward);
    [[nodiscard]] bool ConsumePendingSignal() noexcept;

    [[nodiscard]] static LuaRuntime* FromState(lua_State* state);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace skynet::service::detail

#endif  // SKYNET_PVZG_SERVICE_LUA_RUNTIME_HPP_
