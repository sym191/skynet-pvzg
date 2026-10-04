add_library(skynet_config STATIC
    runtime_config.cc
)

target_include_directories(skynet_config
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_features(skynet_config PUBLIC cxx_std_23)
target_link_libraries(skynet_config PUBLIC skynet::lua_c)

add_library(skynet::config ALIAS skynet_config)
