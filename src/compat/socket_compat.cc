#include "Context.hpp"
#include "OwnedBuffer.hpp"
#include "Runtime.hpp"

#include <arpa/inet.h>

#include <cstdlib>
#include <cstring>
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
                net::SendPriority priority) noexcept {
    if (!buffer) { return -1; }
    // MEMORY transfers even on rejection; RAWPOINTER is borrowed and copied.
    auto owned = buffer->type == SOCKET_BUFFER_MEMORY ?
        skynet::runtime::OwnedBuffer::adopt(const_cast<void*>(buffer->buffer), buffer->sz) :
        skynet::runtime::OwnedBuffer{};
    try {
        auto* value = unwrap(context);
        if (!value || buffer->type == SOCKET_BUFFER_OBJECT ||
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

int skynet_socket_bind(skynet_context* context, int fd) {
    try {
        auto* value = unwrap(context);
        if (!value) { return -1; }
        auto result = value->runtime().sockets().bind_fd(value->handle(), fd);
        return result ? *result : -1;
    } catch (...) { return -1; }
}
int skynet_socket_udp(skynet_context* context, const char* host, int port) {
    try {
        auto* value = unwrap(context);
        if (!value || port < 0 || port > 65535) { return -1; }
        auto result = value->runtime().sockets().udp_bind(value->handle(), host ? host : "", port);
        return result ? *result : -1;
    } catch (...) { return -1; }
}
int skynet_socket_udp_connect(skynet_context* context, int id, const char* host, int port) {
    try {
        auto* value = unwrap(context);
        if (!value || !host || port <= 0 || port > 65535) { return -1; }
        return value->runtime().sockets().udp_connect(value->handle(), id,
            {host, static_cast<std::uint16_t>(port)}) ? 0 : -1;
    } catch (...) { return -1; }
}
int skynet_socket_udp_dial(skynet_context* context, const char* host, int port) {
    int id = skynet_socket_udp(context, nullptr, 0);
    if (id >= 0 && skynet_socket_udp_connect(context, id, host, port)) {
        skynet_socket_close(context, id);
        return -1;
    }
    return id;
}
int skynet_socket_udp_listen(skynet_context* context, const char* host, int port) {
    return port > 0 ? skynet_socket_udp(context, host, port) : -1;
}
int skynet_socket_udp_sendbuffer(skynet_context* context, const char* address, socket_sendbuffer* buffer) {
    if (!buffer) { return -1; }
    auto owned = buffer->type == SOCKET_BUFFER_MEMORY ?
        skynet::runtime::OwnedBuffer::adopt(const_cast<void*>(buffer->buffer), buffer->sz) :
        skynet::runtime::OwnedBuffer{};
    try {
        auto* value = unwrap(context);
        if (!value || !address || static_cast<unsigned char>(address[0]) != 1 ||
            (buffer->type != SOCKET_BUFFER_MEMORY && buffer->type != SOCKET_BUFFER_RAWPOINTER) ||
            (!buffer->buffer && buffer->sz) || buffer->sz > 65507) { return -1; }
        std::uint16_t port;
        std::memcpy(&port, address + 1, sizeof(port));
        char host[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, address + 3, host, sizeof(host))) { return -1; }
        net::Buffer data(buffer->sz);
        if (buffer->sz) { std::memcpy(data.data(), buffer->buffer, buffer->sz); }
        return value->runtime().sockets().udp_send(value->handle(), buffer->id,
            std::move(data), {host, ntohs(port)}) ? 0 : -1;
    } catch (...) { return -1; }
}
const char* skynet_socket_udp_address(skynet_socket_message* message, int* size) {
    if (size) { *size = 0; }
    if (!message || message->type != SKYNET_SOCKET_TYPE_UDP || !message->buffer || message->ud < 0) {
        return nullptr;
    }
    const auto* address = message->buffer + message->ud;
    if (static_cast<unsigned char>(address[0]) != 1) { return nullptr; }
    if (size) { *size = 7; }
    return address;
}
socket_info* skynet_socket_info() {
    socket_info* head = nullptr;
    try {
        auto* context = Context::current();
        if (!context) { return nullptr; }
        for (const auto& entry : context->runtime().sockets().info()) {
            auto* node = static_cast<socket_info*>(std::calloc(1, sizeof(socket_info)));
            if (!node) { socket_info_release(head); return nullptr; }
            node->id = entry.id;
            node->type = static_cast<int>(entry.type);
            node->opaque = entry.owner;
            node->read = entry.read;
            node->write = entry.written;
            node->rtime = entry.read_time;
            node->wtime = entry.write_time;
            node->wbuffer = entry.queued_bytes;
            node->reading = entry.reading;
            node->writing = entry.writing;
            std::strncpy(node->name, entry.name.c_str(), sizeof(node->name) - 1);
            node->next = head;
            head = node;
        }
    } catch (...) { socket_info_release(head); return nullptr; }
    return head;
}
void socket_info_release(socket_info* info) {
    while (info) {
        auto* next = info->next;
        std::free(info);
        info = next;
    }
}
}  // extern "C"
