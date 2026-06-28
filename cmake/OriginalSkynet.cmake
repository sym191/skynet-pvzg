function(add_original_skynet_target)
    set(options)
    set(one_value_args ROOT_DIR BUILD_DIR CSERVICE_DIR LUACLIB_DIR)
    set(multi_value_args)
    cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    foreach(required_arg ROOT_DIR BUILD_DIR CSERVICE_DIR LUACLIB_DIR)
        if(NOT ARG_${required_arg})
            message(FATAL_ERROR "add_original_skynet_target requires ${required_arg}.")
        endif()
    endforeach()

    if(NOT EXISTS "${ARG_ROOT_DIR}/Makefile")
        message(FATAL_ERROR "Original skynet submodule is missing. Run tools/bootstrap.sh first.")
    endif()

    find_program(SKPVZG_MAKE_PROGRAM NAMES make gmake REQUIRED)

    set(original_skynet_binary "${ARG_BUILD_DIR}/skynet")
    set(original_cservices
        "${ARG_CSERVICE_DIR}/snlua.so"
        "${ARG_CSERVICE_DIR}/logger.so"
        "${ARG_CSERVICE_DIR}/gate.so"
        "${ARG_CSERVICE_DIR}/harbor.so"
    )
    set(original_lua_clibs
        "${ARG_LUACLIB_DIR}/skynet.so"
        "${ARG_LUACLIB_DIR}/client.so"
        "${ARG_LUACLIB_DIR}/bson.so"
        "${ARG_LUACLIB_DIR}/md5.so"
        "${ARG_LUACLIB_DIR}/sproto.so"
        "${ARG_LUACLIB_DIR}/lpeg.so"
    )

    add_custom_command(
        OUTPUT "${original_skynet_binary}"
        BYPRODUCTS ${original_cservices} ${original_lua_clibs}
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${ARG_BUILD_DIR}"
                "${ARG_CSERVICE_DIR}"
                "${ARG_LUACLIB_DIR}"
        COMMAND "${SKPVZG_MAKE_PROGRAM}" -C "${ARG_ROOT_DIR}" linux
                "SKYNET_BUILD_PATH=${ARG_BUILD_DIR}"
                "CSERVICE_PATH=${ARG_CSERVICE_DIR}"
                "LUA_CLIB_PATH=${ARG_LUACLIB_DIR}"
                "CC=${CMAKE_C_COMPILER}"
        WORKING_DIRECTORY "${ARG_ROOT_DIR}"
        USES_TERMINAL
        VERBATIM
    )

    add_custom_target(skynet_original DEPENDS "${original_skynet_binary}")
    add_custom_target(skynet_original_clean
        COMMAND "${SKPVZG_MAKE_PROGRAM}" -C "${ARG_ROOT_DIR}" clean
                "SKYNET_BUILD_PATH=${ARG_BUILD_DIR}"
                "CSERVICE_PATH=${ARG_CSERVICE_DIR}"
                "LUA_CLIB_PATH=${ARG_LUACLIB_DIR}"
                "CC=${CMAKE_C_COMPILER}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf
                "${ARG_BUILD_DIR}"
                "${ARG_CSERVICE_DIR}"
                "${ARG_LUACLIB_DIR}"
        WORKING_DIRECTORY "${ARG_ROOT_DIR}"
        USES_TERMINAL
        VERBATIM
    )
endfunction()
