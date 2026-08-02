#pragma once

#include <optional>
#include <string>
#include <string_view>

struct lua_State;

namespace skynet::lua {

class LuaState {
public:
    LuaState();
    ~LuaState();

    LuaState(const LuaState&) = delete;
    LuaState(LuaState&&) = delete;
    LuaState& operator=(const LuaState&) = delete;
    LuaState& operator=(LuaState&&) = delete;

    [[nodiscard]] lua_State* get() const noexcept;
    [[nodiscard]] bool execute(
            std::string_view code,
            std::string_view chunk_name,
            std::string* error = nullptr);
    [[nodiscard]] bool execute_file(
            std::string_view path,
            std::string* error = nullptr);

    void set_global_string(std::string_view name, std::string_view value);
    [[nodiscard]] std::optional<std::string> get_global_string(
            std::string_view name) const;

private:
    void capture_error(std::string* error);

    lua_State* state_ = nullptr;
};

}  // namespace skynet::lua
