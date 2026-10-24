# The unchanged C Lua boundary is intentionally reused against the C++ facade.
# No original scheduler, socket server, or service implementation is linked.
add_library(skynet_lua_service STATIC
    LuaService.cc
    LuaSupport.cc
    NativeService.cc
    ../compat/skynet_compat.cc
    ../compat/socket_compat.cc
    "${PROJECT_SOURCE_DIR}/third_party/skynet/lualib-src/lua-skynet.c"
    "${PROJECT_SOURCE_DIR}/third_party/skynet/lualib-src/lua-seri.c"
    "${PROJECT_SOURCE_DIR}/third_party/skynet/lualib-src/lua-socket.c"
    "${PROJECT_SOURCE_DIR}/third_party/skynet/lualib-src/lua-netpack.c"
)
target_include_directories(skynet_lua_service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/third_party/skynet/skynet-src")
target_link_libraries(skynet_lua_service PUBLIC skynet::runtime skynet::lua_c)
add_library(skynet::lua_service ALIAS skynet_lua_service)

# Native C service ABI; loaded through the existing module loader.
add_library(skynet_gate MODULE gate.cc)
target_include_directories(skynet_gate PRIVATE "${PROJECT_SOURCE_DIR}/third_party/skynet/skynet-src")
set_target_properties(skynet_gate PROPERTIES PREFIX "" OUTPUT_NAME gate
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/refactor/cservice")
