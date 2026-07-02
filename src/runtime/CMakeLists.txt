
add_library(skynet_runtime
    STATIC
    MessageQueue.cc
    Environment.cc
    ServiceRegistry.cc
    HandleManager.cc
    Service.cc
    ServiceRegistry.cc
)

target_include_directories(
    skynet_runtime
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(skynet_runtime
    PUBLIC
    cxx_std_23)

add_library(skynet::runtime ALIAS skynet_runtime)