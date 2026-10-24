#include "SocketEvent.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

extern "C" {
#include "skynet_socket.h"
}

namespace skynet::runtime {
namespace {
void drop_socket_message(void* pointer) {
    auto* message = static_cast<skynet_socket_message*>(pointer);
    std::free(message->buffer);
    std::free(message);
}
}  // namespace

OwnedBuffer make_socket_message(network::Event&& event) {
    int type = 0;
    int value = 0;
    bool padding = false;
    switch (event.type) {
    case network::EventType::data:
        type = SKYNET_SOCKET_TYPE_DATA;
        if (event.payload.size() > std::numeric_limits<int>::max()) {
            throw std::length_error("socket payload exceeds ABI limit");
        }
        value = static_cast<int>(event.payload.size());
        break;
    case network::EventType::open:
        type = SKYNET_SOCKET_TYPE_CONNECT;
        value = static_cast<int>(event.value);
        padding = true;
        // Listen-ready carries the address alone and its port in ud.
        if (value && event.text != "start" && event.text != "transfer") {
            const auto separator = event.text.rfind(':');
            if (separator != std::string::npos) { event.text.resize(separator); }
            if (event.text.starts_with('[') && event.text.ends_with(']')) {
                event.text = event.text.substr(1, event.text.size() - 2);
            }
        }
        break;
    case network::EventType::close: type = SKYNET_SOCKET_TYPE_CLOSE; break;
    case network::EventType::accept:
        type = SKYNET_SOCKET_TYPE_ACCEPT;
        value = event.accepted_socket;
        padding = true;
        break;
    case network::EventType::error:
        type = SKYNET_SOCKET_TYPE_ERROR;
        padding = true;
        break;
    case network::EventType::warning:
        type = SKYNET_SOCKET_TYPE_WARNING;
        value = static_cast<int>(event.value);
        break;
    case network::EventType::udp:
        type = SKYNET_SOCKET_TYPE_UDP;
        if (event.payload.size() > 65507) { throw std::length_error("invalid IPv4 datagram size"); }
        value = static_cast<int>(event.payload.size());
        break;
    }
    const auto text_size = padding ? std::min(event.text.size(), std::size_t{128}) : 0;
    const auto size = sizeof(skynet_socket_message) + text_size;
    auto* message = static_cast<skynet_socket_message*>(std::malloc(size));
    if (!message) { throw std::bad_alloc(); }
    *message = {type, event.socket, value, nullptr};
    auto result = OwnedBuffer::adopt(message, size, drop_socket_message);
    if (padding) {
        std::memcpy(message + 1, event.text.data(), text_size);
    } else if (!event.payload.empty() || type == SKYNET_SOCKET_TYPE_UDP) {
        const auto address_size = type == SKYNET_SOCKET_TYPE_UDP ? 7 : 0;
        message->buffer = static_cast<char*>(std::malloc(event.payload.size() + address_size));
        if (!message->buffer) { throw std::bad_alloc(); }
        if (!event.payload.empty()) {
            std::memcpy(message->buffer, event.payload.data(), event.payload.size());
        }
        if (address_size) {
            auto* address = message->buffer + value;
            address[0] = 1; // Upstream PROTOCOL_UDP, network-order port + IPv4.
            const auto port = htons(event.port);
            std::memcpy(address + 1, &port, sizeof(port));
            if (inet_pton(AF_INET, event.text.c_str(), address + 3) != 1) {
                throw std::invalid_argument("invalid UDP source address");
            }
        }
    }
    return result;
}

void handoff_socket_message(OwnedBuffer& buffer) noexcept {
    if (buffer.deleter() == drop_socket_message) { buffer.set_deleter(std::free); }
}
}  // namespace skynet::runtime
