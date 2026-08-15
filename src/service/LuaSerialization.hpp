#ifndef SKYNET_PVZG_SERVICE_LUA_SERIALIZATION_HPP_
#define SKYNET_PVZG_SERVICE_LUA_SERIALIZATION_HPP_

struct lua_State;

namespace skynet::service::detail {

[[nodiscard]] int LuaPack(lua_State* state);
[[nodiscard]] int LuaUnpack(lua_State* state);

}  // namespace skynet::service::detail

#endif  // SKYNET_PVZG_SERVICE_LUA_SERIALIZATION_HPP_
