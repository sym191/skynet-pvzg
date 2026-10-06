#include "Context.hpp"
#include "OwnedBuffer.hpp"
#include "Runtime.hpp"

#include <cstdlib>
#include <span>

extern "C" {
#include "skynet_socket.h"
}

namespace {
using skynet::runtime::Context;
namespace net = skynet::network;
Context* unwrap(skynet_context* context) noexcept {
    return context ? reinterpret_cast<Context*>(context) : Context::current();
}

int send_buffer(skynet_context* context, socket_sendbuffer* buffer,
                net::SendPriority priority, bool supported = true) noexcept {
    if (!buffer) { return -1; }
    // MEMORY transfers even on rejection; RAWPOINTER is borrowed and copied.
    auto owned = buffer->type == SOCKET_BUFFER_MEMORY ?
        skynet::runtime::OwnedBuffer::adopt(const_cast<void*>(buffer->buffer), buffer->sz) :
        skynet::runtime::OwnedBuffer{};
    try {
        auto* value = unwrap(context);
        if (!supported || !value || buffer->type == SOCKET_BUFFER_OBJECT ||
            (buffer->type != SOCKET_BUFFER_MEMORY && buffer->type != SOCKET_BUFFER_RAWPOINTER) ||
            (!buffer->buffer && buffer->sz != 0)) { return -1; }
        const auto data = std::span(static_cast<const std::byte*>(buffer->buffer), buffer->sz);
        return value->runtime().sockets().send(value->handle(), buffer->id, data, priority) ? 0 : -1;
    } catch (...) { return -1; }
}

template <class Operation>
void operate(skynet_context* context, int id, Operation operation) noexcept {
    try {
        if (auto* value = unwrap(context)) {
            (void)operation(value->runtime().sockets(), value->handle(), id);
        }
    } catch (...) {}
}
}  // namespace

extern "C" {
int skynet_socket_sendbuffer(skynet_context* context, socket_sendbuffer* buffer) {
    return send_buffer(context, buffer, net::SendPriority::high);
}
int skynet_socket_sendbuffer_lowpriority(skynet_context* context, socket_sendbuffer* buffer) {
    return send_buffer(context, buffer, net::SendPriority::low);
}
int skynet_socket_listen(skynet_context* context, const char* host, int port, int backlog) {
    try {
        auto* value = unwrap(context);
        if (!value || !host || port < 0 || port > 65535) { return -1; }
        auto result = value->runtime().sockets().listen(value->handle(), host, static_cast<std::uint16_t>(port), backlog);
        return result ? *result : -1;
    } catch (...) { return -1; }
}
int skynet_socket_connect(skynet_context* context, const char* host, int port) {
    try {
        auto* value = unwrap(context);
        if (!value || !host || port <= 0 || port > 65535) { return -1; }
        auto result = value->runtime().sockets().connect(value->handle(), host, static_cast<std::uint16_t>(port));
        return result ? *result : -1;
    } catch (...) { return -1; }
}
void skynet_socket_close(skynet_context* context, int id) {
    operate(context, id, [](auto& runtime, auto owner, auto socket) { return runtime.close_socket(owner, socket); });
}
void skynet_socket_shutdown(skynet_context* context, int id) {
    operate(context, id, [](auto& runtime, auto owner, auto socket) { return runtime.shutdown_socket(owner, socket); });
}
void skynet_socket_start(skynet_context* context, int id) {
    operate(context, id, [](auto& runtime, auto owner, auto socket) { return runtime.start_socket(owner, socket); });
}
void skynet_socket_pause(skynet_context* context, int id) {
    operate(context, id, [](auto& runtime, auto owner, auto socket) { return runtime.pause_socket(owner, socket); });
}
void skynet_socket_nodelay(skynet_context* context, int id) {
    operate(context, id, [](auto& runtime, auto owner, auto socket) { return runtime.set_nodelay(owner, socket); });
}

// Stage 5 APIs fail explicitly. No original socket_server is linked.
int skynet_socket_bind(skynet_context*, int) { return -1; }
int skynet_socket_udp(skynet_context*, const char*, int) { return -1; }
int skynet_socket_udp_connect(skynet_context*, int, const char*, int) { return -1; }
int skynet_socket_udp_dial(skynet_context*, const char*, int) { return -1; }
int skynet_socket_udp_listen(skynet_context*, const char*, int) { return -1; }
int skynet_socket_udp_sendbuffer(skynet_context* context, const char*, socket_sendbuffer* buffer) {
    return send_buffer(context, buffer, net::SendPriority::high, false);
}
const char* skynet_socket_udp_address(skynet_socket_message*, int* size) {
    if (size) { *size = 0; }
    return nullptr;
}
socket_info* skynet_socket_info() { return nullptr; }
void socket_info_release(socket_info* info) {
    while (info) {
        auto* next = info->next;
        std::free(info);
        info = next;
    }
}
}  // extern "C"
