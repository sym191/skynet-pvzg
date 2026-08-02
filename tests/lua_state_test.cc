#include "lua_state.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    skynet::lua::LuaState state;
    if (state.get() == nullptr) {
        return fail("Lua state was not created");
    }

    std::string error;
    if (!state.execute("answer = 'pvzg'; return 1 + 1", "lua_state_test", &error)) {
        return fail(error);
    }

    const auto answer = state.get_global_string("answer");
    if (!answer || *answer != "pvzg") {
        return fail("Lua global string was not read back");
    }

    state.set_global_string("from_cpp", "hello");
    if (!state.execute("from_lua = from_cpp .. ' lua'", "lua_state_test", &error)) {
        return fail(error);
    }

    const auto from_lua = state.get_global_string("from_lua");
    if (!from_lua || *from_lua != "hello lua") {
        return fail("Lua global string was not written from C++");
    }

    if (state.execute("error('expected failure')", "lua_state_error_test", &error)) {
        return fail("Lua error path should fail");
    }
    if (error.find("expected failure") == std::string::npos) {
        return fail("Lua error message did not include the script error");
    }

    return EXIT_SUCCESS;
}
