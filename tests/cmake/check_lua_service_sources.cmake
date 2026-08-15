if(NOT DEFINED COMPILE_COMMANDS)
    message(FATAL_ERROR "COMPILE_COMMANDS is required")
endif()

file(READ "${COMPILE_COMMANDS}" compile_commands)

set(forbidden_sources
    "service-src/service_snlua.c"
    "lualib-src/lua-skynet.c"
    "lualib-src/lua-seri.c"
)

foreach(source IN LISTS forbidden_sources)
    string(FIND "${compile_commands}" "${source}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR
            "skynet_lua_service still compiles original Skynet source: ${source}")
    endif()
endforeach()

foreach(source IN ITEMS LuaCore.cc LuaRuntime.cc LuaSerialization.cc)
    string(FIND "${compile_commands}" "/src/service/${source}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "missing C++ Lua service implementation in build: ${source}")
    endif()
endforeach()
