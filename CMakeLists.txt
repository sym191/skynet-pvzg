cmake_minimum_required(VERSION 3.28)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

project(
    skynet-pvzg
    VERSION 0.1.0
    DESCRIPTION "C++23 rewrite of Skynet"
    LANGUAGES C CXX
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "skynet-pvzg only supports Linux.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(ProjectDependencies)
include(OriginalSkynet)

set(SKPVZG_SHARED_LUA_ROOT "${PROJECT_SOURCE_DIR}/lua")
set(SKPVZG_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(SKPVZG_GENERATED_CONFIG_DIR "${SKPVZG_GENERATED_DIR}/configs")
set(SKPVZG_ORIGINAL_OUTPUT_DIR "${CMAKE_BINARY_DIR}/original")
set(SKPVZG_ORIGINAL_BIN_DIR "${SKPVZG_ORIGINAL_OUTPUT_DIR}/bin")
set(SKPVZG_ORIGINAL_CSERVICE_DIR "${SKPVZG_ORIGINAL_OUTPUT_DIR}/cservice")
set(SKPVZG_ORIGINAL_LUACLIB_DIR "${SKPVZG_ORIGINAL_OUTPUT_DIR}/luaclib")
set(SKPVZG_REFACTOR_OUTPUT_DIR "${CMAKE_BINARY_DIR}/refactor")
set(SKPVZG_REFACTOR_BIN_DIR "${SKPVZG_REFACTOR_OUTPUT_DIR}/bin")

file(MAKE_DIRECTORY
    "${SKPVZG_GENERATED_CONFIG_DIR}"
    "${SKPVZG_ORIGINAL_BIN_DIR}"
    "${SKPVZG_ORIGINAL_CSERVICE_DIR}"
    "${SKPVZG_ORIGINAL_LUACLIB_DIR}"
    "${SKPVZG_REFACTOR_BIN_DIR}"
)

configure_file(
    "${PROJECT_SOURCE_DIR}/configs/skynet_original.lua.in"
    "${SKPVZG_GENERATED_CONFIG_DIR}/skynet_original.lua"
    @ONLY
)
configure_file(
    "${PROJECT_SOURCE_DIR}/configs/skynet_refactor.lua.in"
    "${SKPVZG_GENERATED_CONFIG_DIR}/skynet_refactor.lua"
    @ONLY
)

add_original_skynet_target(
    ROOT_DIR "${PROJECT_SOURCE_DIR}/third_party/skynet"
    BUILD_DIR "${SKPVZG_ORIGINAL_BIN_DIR}"
    CSERVICE_DIR "${SKPVZG_ORIGINAL_CSERVICE_DIR}"
    LUACLIB_DIR "${SKPVZG_ORIGINAL_LUACLIB_DIR}"
)

add_subdirectory(src/network)
add_subdirectory(src/lua)
add_subdirectory(src/config)
add_subdirectory(src/runtime)
add_subdirectory(src/service)

add_executable(skynet_refactor src/skynet_refactor/main.cc)
target_link_libraries(skynet_refactor
    PRIVATE
        asio::asio
        spdlog::spdlog
        skynet::config
        skynet::runtime
        skynet::lua_service
)
set_target_properties(skynet_refactor PROPERTIES
    ENABLE_EXPORTS ON
    OUTPUT_NAME skynet
    RUNTIME_OUTPUT_DIRECTORY "${SKPVZG_REFACTOR_BIN_DIR}"
)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_library(skpvzg_native_probe MODULE tests/native_probe.c)
target_include_directories(skpvzg_native_probe PRIVATE "${PROJECT_SOURCE_DIR}/third_party/skynet/skynet-src")
set_target_properties(skpvzg_native_probe PROPERTIES PREFIX "" OUTPUT_NAME nativeprobe
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-cservice")
add_library(skpvzg_native_logger MODULE tests/native_logger.c)
target_include_directories(skpvzg_native_logger PRIVATE "${PROJECT_SOURCE_DIR}/third_party/skynet/skynet-src")
set_target_properties(skpvzg_native_logger PROPERTIES PREFIX "" OUTPUT_NAME nativelogger
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-cservice")
add_dependencies(skynet_refactor skpvzg_native_probe skpvzg_native_logger skynet_gate)

foreach(STAGE3_VARIANT IN ITEMS original refactor)
    set(STAGE3_CSERVICE "${CMAKE_BINARY_DIR}/${STAGE3_VARIANT}/cservice")
    set(STAGE3_LUACLIB "${CMAKE_BINARY_DIR}/${STAGE3_VARIANT}/luaclib")
    configure_file(tests/cmake/stage3_config.lua.in
        "${SKPVZG_GENERATED_CONFIG_DIR}/stage3-${STAGE3_VARIANT}.lua" @ONLY)
    configure_file(tests/cmake/stage4_config.lua.in
        "${SKPVZG_GENERATED_CONFIG_DIR}/stage4-${STAGE3_VARIANT}.lua" @ONLY)
    configure_file(tests/cmake/stage5_config.lua.in
        "${SKPVZG_GENERATED_CONFIG_DIR}/stage5-${STAGE3_VARIANT}.lua" @ONLY)
    add_test(NAME "lua.stage5.${STAGE3_VARIANT}" COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/run_stage5.py"
        "${CMAKE_BINARY_DIR}/${STAGE3_VARIANT}/bin/skynet"
        "${SKPVZG_GENERATED_CONFIG_DIR}/stage5-${STAGE3_VARIANT}.lua")
    set_tests_properties("lua.stage5.${STAGE3_VARIANT}" PROPERTIES TIMEOUT 65)
    add_test(NAME "lua.stage4.${STAGE3_VARIANT}" COMMAND "${CMAKE_COMMAND}"
        "-DSKYNET=${CMAKE_BINARY_DIR}/${STAGE3_VARIANT}/bin/skynet"
        "-DCONFIG=${SKPVZG_GENERATED_CONFIG_DIR}/stage4-${STAGE3_VARIANT}.lua"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/run_stage4.cmake")
    set_tests_properties("lua.stage4.${STAGE3_VARIANT}" PROPERTIES TIMEOUT 65)
    add_test(NAME "lua.stage3.${STAGE3_VARIANT}" COMMAND "${CMAKE_COMMAND}"
        "-DSKYNET=${CMAKE_BINARY_DIR}/${STAGE3_VARIANT}/bin/skynet"
        "-DCONFIG=${SKPVZG_GENERATED_CONFIG_DIR}/stage3-${STAGE3_VARIANT}.lua"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/run_stage3.cmake")
    set_tests_properties("lua.stage3.${STAGE3_VARIANT}" PROPERTIES TIMEOUT 20)
    add_test(NAME "lua.custom_logger.${STAGE3_VARIANT}" COMMAND "${CMAKE_COMMAND}"
        "-DVARIANT=${STAGE3_VARIANT}"
        "-DSKYNET=${CMAKE_BINARY_DIR}/${STAGE3_VARIANT}/bin/skynet"
        "-DCONFIG=${SKPVZG_GENERATED_CONFIG_DIR}/stage3-${STAGE3_VARIANT}.lua"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/run_custom_logger.cmake")
    set_tests_properties("lua.custom_logger.${STAGE3_VARIANT}" PROPERTIES TIMEOUT 20)
endforeach()
add_test(NAME lua.bootstrap_failure COMMAND "${CMAKE_COMMAND}"
    "-DSKYNET=$<TARGET_FILE:skynet_refactor>"
    "-DCONFIG=${SKPVZG_GENERATED_CONFIG_DIR}/stage3-refactor.lua"
    -P "${PROJECT_SOURCE_DIR}/tests/cmake/run_bootstrap_failure.cmake")
set_tests_properties(lua.bootstrap_failure PROPERTIES TIMEOUT 10)

add_executable(skpvzg_dependency_smoke tests/dependency_smoke_test.cc)
target_link_libraries(skpvzg_dependency_smoke
    PRIVATE
        asio::asio
        spdlog::spdlog
)

add_test(NAME dependencies.smoke COMMAND skpvzg_dependency_smoke)

add_executable(skpvzg_runtime_log_test tests/runtime_log_test.cc)
target_link_libraries(skpvzg_runtime_log_test
    PRIVATE
        skynet::runtime
)

add_test(NAME runtime.log COMMAND skpvzg_runtime_log_test)

add_executable(skpvzg_runtime_config_test tests/runtime_config_test.cc)
target_compile_definitions(skpvzg_runtime_config_test
    PRIVATE
        SKPVZG_REFACTOR_CONFIG="${SKPVZG_GENERATED_CONFIG_DIR}/skynet_refactor.lua"
)
target_link_libraries(skpvzg_runtime_config_test PRIVATE skynet::config)
add_test(NAME runtime.config COMMAND skpvzg_runtime_config_test)

add_executable(skpvzg_runtime_core_test tests/runtime_core_test.cc)
target_link_libraries(skpvzg_runtime_core_test PRIVATE skynet::runtime)
add_test(NAME runtime.core COMMAND skpvzg_runtime_core_test)
add_executable(skpvzg_runtime_boundary_test tests/runtime_boundary_test.cc)
target_link_libraries(skpvzg_runtime_boundary_test PRIVATE skynet::runtime)
add_test(NAME runtime.boundary COMMAND skpvzg_runtime_boundary_test)

add_executable(skpvzg_runtime_monitor_test tests/runtime_monitor_test.cc)
target_link_libraries(skpvzg_runtime_monitor_test PRIVATE skynet::runtime)
add_test(NAME runtime.monitor COMMAND skpvzg_runtime_monitor_test)
set_tests_properties(runtime.monitor PROPERTIES TIMEOUT 20)

add_executable(skpvzg_runtime_contract_test tests/runtime_contract_test.cc)
target_link_libraries(skpvzg_runtime_contract_test PRIVATE skynet::lua_service)
add_test(NAME runtime.contract COMMAND skpvzg_runtime_contract_test)
add_executable(skpvzg_network_runtime_test tests/network_runtime_test.cc)
target_link_libraries(skpvzg_network_runtime_test PRIVATE skynet::network)
add_test(NAME network.runtime COMMAND skpvzg_network_runtime_test)
add_executable(skpvzg_network_stage5_test tests/network_stage5_test.cc)
target_link_libraries(skpvzg_network_stage5_test PRIVATE skynet::network)
add_test(NAME network.stage5 COMMAND skpvzg_network_stage5_test)
set_tests_properties(network.stage5 PROPERTIES TIMEOUT 30)
add_executable(skpvzg_runtime_socket_test tests/runtime_socket_test.cc)
target_include_directories(skpvzg_runtime_socket_test PRIVATE "${PROJECT_SOURCE_DIR}/third_party/skynet/skynet-src")
target_link_libraries(skpvzg_runtime_socket_test PRIVATE skynet::runtime)
add_test(NAME runtime.socket COMMAND skpvzg_runtime_socket_test)
set_tests_properties(runtime.contract runtime.socket network.runtime PROPERTIES TIMEOUT 30)

add_test(
    NAME configs.generated
    COMMAND
        "${CMAKE_COMMAND}"
        "-DORIGINAL_CONFIG=${SKPVZG_GENERATED_CONFIG_DIR}/skynet_original.lua"
        "-DREFACTOR_CONFIG=${SKPVZG_GENERATED_CONFIG_DIR}/skynet_refactor.lua"
        "-DSHARED_LUA_ROOT=${SKPVZG_SHARED_LUA_ROOT}"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/check_generated_config.cmake"
)

add_test(
    NAME original.outputs
    COMMAND
        "${CMAKE_COMMAND}"
        "-DORIGINAL_SKYNET=${SKPVZG_ORIGINAL_BIN_DIR}/skynet"
        "-DORIGINAL_CSERVICE=${SKPVZG_ORIGINAL_CSERVICE_DIR}/logger.so"
        "-DORIGINAL_LUACLIB=${SKPVZG_ORIGINAL_LUACLIB_DIR}/skynet.so"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/check_original_outputs.cmake"
)

add_custom_target(skpvzg_tests DEPENDS
    skpvzg_dependency_smoke
    skpvzg_runtime_log_test
    skpvzg_runtime_config_test
    skpvzg_runtime_core_test
    skpvzg_runtime_boundary_test
    skpvzg_runtime_monitor_test
    skpvzg_runtime_contract_test
    skpvzg_network_runtime_test
    skpvzg_network_stage5_test
    skpvzg_runtime_socket_test
)
add_custom_target(skynet_all DEPENDS skynet_refactor skynet_original skynet_network skpvzg_tests)
