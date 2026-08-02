set(SKPVZG_LUA_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third_party/skynet/3rd/lua")

add_library(skynet_lua_c STATIC
    "${SKPVZG_LUA_SOURCE_DIR}/lapi.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lauxlib.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lbaselib.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lcode.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lcorolib.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lctype.c"
    "${SKPVZG_LUA_SOURCE_DIR}/ldblib.c"
    "${SKPVZG_LUA_SOURCE_DIR}/ldebug.c"
    "${SKPVZG_LUA_SOURCE_DIR}/ldo.c"
    "${SKPVZG_LUA_SOURCE_DIR}/ldump.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lfunc.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lgc.c"
    "${SKPVZG_LUA_SOURCE_DIR}/linit.c"
    "${SKPVZG_LUA_SOURCE_DIR}/liolib.c"
    "${SKPVZG_LUA_SOURCE_DIR}/llex.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lmathlib.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lmem.c"
    "${SKPVZG_LUA_SOURCE_DIR}/loadlib.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lobject.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lopcodes.c"
    "${SKPVZG_LUA_SOURCE_DIR}/loslib.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lparser.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lstate.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lstring.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lstrlib.c"
    "${SKPVZG_LUA_SOURCE_DIR}/ltable.c"
    "${SKPVZG_LUA_SOURCE_DIR}/ltablib.c"
    "${SKPVZG_LUA_SOURCE_DIR}/ltm.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lundump.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lutf8lib.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lvm.c"
    "${SKPVZG_LUA_SOURCE_DIR}/lzio.c"
)

target_include_directories(skynet_lua_c
    PUBLIC
        "${SKPVZG_LUA_SOURCE_DIR}"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/skynet/skynet-src"
)

target_compile_definitions(skynet_lua_c
    PUBLIC
        LUA_USE_LINUX
)

target_compile_features(skynet_lua_c
    PUBLIC
        c_std_11
)

target_link_libraries(skynet_lua_c
    PUBLIC
        m
        ${CMAKE_DL_LIBS}
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(skynet_lua_c
        INTERFACE
            "LINKER:-E"
    )
endif()

add_library(skynet::lua_c ALIAS skynet_lua_c)

add_library(skynet_lua STATIC
    lua_state.cc
)

target_include_directories(skynet_lua
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_features(skynet_lua
    PUBLIC
        cxx_std_23
)

target_link_libraries(skynet_lua
    PUBLIC
        skynet::lua_c
)

add_library(skynet::lua ALIAS skynet_lua)
