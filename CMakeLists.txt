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

add_subdirectory(src/runtime)

add_executable(skynet_refactor src/skynet_refactor/main.cc)

target_link_libraries(skynet_refactor
    PRIVATE
    skynet::runtime)

target_link_libraries(skynet_refactor
    PRIVATE
        asio::asio
        spdlog::spdlog
)
set_target_properties(skynet_refactor PROPERTIES
    OUTPUT_NAME skynet
    RUNTIME_OUTPUT_DIRECTORY "${SKPVZG_REFACTOR_BIN_DIR}"
)

enable_testing()

add_executable(skpvzg_dependency_smoke tests/dependency_smoke_test.cc)
target_link_libraries(skpvzg_dependency_smoke
    PRIVATE
        asio::asio
        spdlog::spdlog
)

add_test(NAME dependencies.smoke COMMAND skpvzg_dependency_smoke)

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

add_custom_target(skpvzg_tests DEPENDS skpvzg_dependency_smoke)
add_custom_target(skynet_all DEPENDS skynet_refactor skynet_original skpvzg_tests)
