add_library(skynet_lua_service STATIC
    LuaService.cc
    skynet_compat.cc
    skynet_snlua_bridge.c
    "${PROJECT_SOURCE_DIR}/third_party/skynet/lualib-src/lua-seri.c"
    "${PROJECT_SOURCE_DIR}/third_party/skynet/lualib-src/lua-skynet.c"
)

target_include_directories(skynet_lua_service
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/skynet/lualib-src"
        "${PROJECT_SOURCE_DIR}/third_party/skynet/service-src"
        "${PROJECT_SOURCE_DIR}/third_party/skynet/skynet-src"
)

target_compile_features(skynet_lua_service
    PUBLIC
        cxx_std_23
    PRIVATE
        c_std_11
)

target_link_libraries(skynet_lua_service
    PUBLIC
        skynet::runtime
        skynet::lua_c
)

add_library(skynet::lua_service ALIAS skynet_lua_service)
