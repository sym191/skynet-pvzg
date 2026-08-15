add_library(skynet_lua_service STATIC
    LuaCore.cc
    LuaRuntime.cc
    LuaSerialization.cc
    LuaService.cc
)

target_include_directories(skynet_lua_service
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/lua"
)

target_compile_features(skynet_lua_service
    PUBLIC
        cxx_std_23
)

target_link_libraries(skynet_lua_service
    PUBLIC
        skynet::runtime
        skynet::lua_c
)

add_library(skynet::lua_service ALIAS skynet_lua_service)
