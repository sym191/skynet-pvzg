add_library(skynet_network STATIC
    core.cc
    tcp_connection.cc
    tcp_listener.cc
)

target_include_directories(skynet_network
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# asio is provided by ProjectDependencies (asio::asio)
if(TARGET asio::asio)
    target_link_libraries(skynet_network PUBLIC asio::asio)
endif()

if(TARGET spdlog::spdlog)
    target_link_libraries(skynet_network PUBLIC spdlog::spdlog)
endif()

set_target_properties(skynet_network PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)
