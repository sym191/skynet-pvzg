#include "Context.hpp"
#include "Runtime.hpp"
#include "ThreadClock.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>

extern "C" {
#include "skynet.h"
#include "skynet_timer.h"
}

namespace {
using skynet::runtime::Context;
using skynet::runtime::Message;
using skynet::runtime::MessageBuffer;
Context* unwrap(skynet_context* context) noexcept {
    return context ? reinterpret_cast<Context*>(context) : Context::current();
}
constexpr std::size_t kMaximumPayload = std::numeric_limits<std::size_t>::max() >> 8;

int send(Context* context, uint32_t source, uint32_t destination, const char* name,
         int type, int session, void* data, std::size_t size) noexcept {
    // adopt before validation: DONTCOPY transfers ownership even on failure.
    auto payload = type & PTYPE_TAG_DONTCOPY ? MessageBuffer::adopt(data, size) : MessageBuffer{};
    try {
        if (size > kMaximumPayload) { return -2; }
        if (!context || (!data && size != 0)) { return -1; }
        bool global = name && name[0] != '.' && name[0] != ':';
        if (name && !global) {
            destination = context->runtime().resolve(name);
            if (destination == 0 && name[0] == '.') { return -1; }
        }
        if (type & PTYPE_TAG_ALLOCSESSION) { session = context->new_session(); }
        if (!global && destination == 0) { return data ? -1 : session; }
        if (!(type & PTYPE_TAG_DONTCOPY)) { payload = MessageBuffer::copy(data, size); }
        Message message{.source = source ? source : context->handle(), .session = session,
            .type = static_cast<skynet::runtime::ProtocolType>(type & 0xff), .data = std::move(payload)};
        const bool sent = global ? context->runtime().send_global(name, std::move(message)) :
            context->runtime().send(destination, std::move(message));
        return sent ? session : -1;
    } catch (...) { return -1; }
}
}  // namespace

extern "C" {
int skynet_send(skynet_context* context, uint32_t source, uint32_t destination,
                int type, int session, void* data, size_t size) {
    return send(unwrap(context), source, destination, nullptr, type, session, data, size);
}
int skynet_sendname(skynet_context* context, uint32_t source, const char* name,
                    int type, int session, void* data, size_t size) {
    return send(unwrap(context), source, 0, name ? name : "", type, session, data, size);
}
const char* skynet_command(skynet_context* context, const char* command, const char* parameter) {
    try {
        auto* value = unwrap(context);
        return value && command ? value->native_command(command, parameter) : nullptr;
    } catch (...) { return nullptr; }
}
void skynet_callback(skynet_context* context, void* userdata, skynet_cb callback) {
    if (auto* value = unwrap(context)) { value->set_callback(userdata, callback); }
}
uint32_t skynet_queryname(skynet_context* context, const char* name) {
    try { auto* value = unwrap(context); return value && name ? value->runtime().resolve(name) : 0; }
    catch (...) { return 0; }
}
int skynet_isremote(skynet_context* context, uint32_t handle, int* harbor) {
    if (auto* value = unwrap(context)) { return value->is_remote(handle, harbor); }
    if (harbor) { *harbor = static_cast<int>(handle >> 24); }
    return 0;
}
uint32_t skynet_current_handle() { auto* value = Context::current(); return value ? value->handle() : 0; }
uint64_t skynet_now() { auto* value = Context::current(); return value ? value->now_ticks() : 0; }
uint32_t skynet_starttime() { auto* value = Context::current(); return value ? value->runtime().start_time() : 0; }
uint64_t skynet_thread_time() { return skynet::runtime::thread_microseconds(); }
void skynet_error(skynet_context* context, const char* format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int size = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    try {
        if (size >= 0) {
            std::string buffer(static_cast<std::size_t>(size) + 1, '\0');
            std::vsnprintf(buffer.data(), buffer.size(), format, args);
            if (auto* value = unwrap(context)) { value->log(std::string_view(buffer.data(), size)); }
            else { std::fprintf(stderr, "%s\n", buffer.c_str()); }
        }
    } catch (...) {}
    va_end(args);
}
void skynet_debug_memory(const char* info) { skynet_error(nullptr, "%s", info ? info : ""); }
void* skynet_lalloc(void* pointer, size_t, size_t size) {
    if (size == 0) { std::free(pointer); return nullptr; }
    return std::realloc(pointer, size);
}
}  // extern "C"
