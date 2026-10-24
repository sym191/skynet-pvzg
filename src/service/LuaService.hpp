#pragma once

#include "Service.hpp"

#include <atomic>
#include <string>
#include <utility>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace skynet::service {
class LuaService final : public runtime::Service {
public:
    ~LuaService() override;
    bool init(runtime::Context& context, std::string_view arguments) override;
    void dispatch(runtime::Context& context, runtime::Message message) override;
    void signal(runtime::Context& context, int value) override;

private:
    static void* allocate(void* user, void* pointer, std::size_t old_size, std::size_t new_size);
    static void interrupt_hook(lua_State* state, lua_Debug* debug);
    static int initialize_lua(lua_State* state);
    static int dispatch_lua(lua_State* state);
    runtime::Message* current_message_{nullptr};
    bool reserved_message_{false};
    lua_State* state_{nullptr};
    runtime::Context* context_{nullptr};
    std::atomic<bool> interrupt_{false};
    std::atomic<std::size_t> memory_{0};
    std::size_t memory_limit_{0};
    std::vector<std::pair<std::string, std::string>> globals_;
    std::string loader_;
    std::string arguments_;
};
}  // namespace skynet::service
