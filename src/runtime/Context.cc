#include "Context.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>

namespace skynet::runtime {
namespace {

constexpr int kMaximumSession = 0x7fff'ffff;
const auto kRuntimeStartTime = std::chrono::system_clock::now();

[[nodiscard]] std::string handle_address(Handle handle) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), ":%x", handle);
    return buffer;
}

[[nodiscard]] std::optional<Handle> parse_handle(std::string_view address) {
    if (address.size() < 2 || address.front() != ':') {
        return std::nullopt;
    }
    Handle handle = 0;
    const char* begin = address.data() + 1;
    const char* end = address.data() + address.size();
    const auto [position, error] = std::from_chars(begin, end, handle, 16);
    if (error != std::errc{} || position != end || handle == 0) {
        return std::nullopt;
    }
    return handle;
}

[[nodiscard]] std::uint32_t parse_ticks(std::optional<std::string_view> value) {
    if (!value) {
        return 0;
    }
    std::uint32_t ticks = 0;
    const auto [position, error] = std::from_chars(
        value->data(), value->data() + value->size(), ticks);
    if (error != std::errc{} || position != value->data() + value->size()) {
        return 0;
    }
    return ticks;
}

}  // namespace

Context::Context(Handle handle)
        : handle_(handle),
          start_time_(kRuntimeStartTime),
          message_queue_(handle) {}

Handle Context::handle() const noexcept {
    return handle_;
}

void Context::set_handle(Handle handle) noexcept {
    handle_ = handle;
    message_queue_.set_owner(handle);
}

int Context::new_session() noexcept {
    int current = next_session_.load(std::memory_order_relaxed);
    while (true) {
        const int next = current == kMaximumSession ? 1 : current + 1;
        if (next_session_.compare_exchange_weak(
                current, next, std::memory_order_relaxed)) {
            return current;
        }
    }
}

int Context::send(
        Handle destination,
        Handle source,
        int type,
        int session,
        std::span<const std::byte> payload,
        bool allocate_session) {
    if (allocate_session) {
        session = new_session();
    }
    if (destination == 0) {
        return payload.empty() ? session : -1;
    }

    Message message;
    message.source = source == 0 ? handle_ : source;
    message.session = session;
    message.type = static_cast<MessageType>(type & 0xff);
    message.data.assign(payload.begin(), payload.end());

    if (send_handler_) {
        return send_handler_(destination, std::move(message)) ? session : -1;
    }
    if (destination == handle_) {
        (void)message_queue_.push(std::move(message));
        return session;
    }
    return -1;
}

int Context::send(
        std::string_view destination,
        Handle source,
        int type,
        int session,
        std::span<const std::byte> payload,
        bool allocate_session) {
    const Handle destination_handle = resolve(destination);
    if (destination_handle == 0) {
        return -1;
    }
    return send(
        destination_handle, source, type, session, payload, allocate_session);
}

Handle Context::resolve(std::string_view address) const {
    if (const auto parsed = parse_handle(address)) {
        return *parsed;
    }
    if (resolve_handler_) {
        if (const Handle resolved = resolve_handler_(address); resolved != 0) {
            return resolved;
        }
    }
    if (!address.empty() && address.front() == '.') {
        const auto iterator = local_names_.find(std::string(address.substr(1)));
        return iterator == local_names_.end() ? 0 : iterator->second;
    }
    return 0;
}

bool Context::is_remote(Handle handle, int* harbor) const {
    const int destination_harbor =
        static_cast<int>(handle >> kHandleRemoteShift);
    if (harbor != nullptr) {
        *harbor = destination_harbor;
    }
    return destination_harbor != 0 &&
        destination_harbor != static_cast<int>(handle_ >> kHandleRemoteShift);
}

std::optional<std::string> Context::command(
        std::string_view command_name,
        std::optional<std::string_view> parameter) {
    if (command_handler_) {
        auto result = command_handler_(command_name, parameter);
        if (result || command_name == "LAUNCH") {
            return result;
        }
    }
    return builtin_command(command_name, parameter);
}

void Context::log(std::string_view message) const {
    if (log_handler_) {
        log_handler_(message);
        return;
    }
    std::cerr << message << '\n';
}

Environment& Context::environment() noexcept {
    return environment_;
}

const Environment& Context::environment() const noexcept {
    return environment_;
}

MessageQueue& Context::message_queue() noexcept {
    return message_queue_;
}

const MessageQueue& Context::message_queue() const noexcept {
    return message_queue_;
}

void Context::record_dispatch() noexcept {
    message_count_.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t Context::message_count() const noexcept {
    return message_count_.load(std::memory_order_relaxed);
}

bool Context::exit_requested() const noexcept {
    return exit_requested_.load(std::memory_order_acquire);
}

void Context::set_send_handler(SendHandler handler) {
    send_handler_ = std::move(handler);
}

void Context::set_resolve_handler(ResolveHandler handler) {
    resolve_handler_ = std::move(handler);
}

void Context::set_name_handler(NameHandler handler) {
    name_handler_ = std::move(handler);
}

void Context::set_command_handler(CommandHandler handler) {
    command_handler_ = std::move(handler);
}

void Context::set_log_handler(LogHandler handler) {
    log_handler_ = std::move(handler);
}

void Context::set_timeout_handler(TimeoutHandler handler) {
    timeout_handler_ = std::move(handler);
}

std::optional<std::string> Context::builtin_command(
        std::string_view command_name,
        std::optional<std::string_view> parameter) {
    if (command_name == "REG") {
        if (!parameter || parameter->empty()) {
            return handle_address(handle_);
        }
        if (parameter->front() == '.' &&
                register_name(parameter->substr(1), handle_)) {
            return std::string(*parameter);
        }
        return std::nullopt;
    }
    if (command_name == "QUERY" && parameter) {
        const Handle resolved = resolve(*parameter);
        return resolved == 0
            ? std::nullopt
            : std::optional<std::string>(handle_address(resolved));
    }
    if (command_name == "NAME" && parameter) {
        const std::size_t separator = parameter->find_first_of(" \t");
        if (separator == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view name = parameter->substr(0, separator);
        const std::string_view address = parameter->substr(separator + 1);
        const auto parsed = parse_handle(address);
        if (name.empty() || name.front() != '.' || !parsed ||
                !register_name(name.substr(1), *parsed)) {
            return std::nullopt;
        }
        return std::string(name);
    }
    if (command_name == "GETENV" && parameter) {
        return environment_.get(*parameter);
    }
    if (command_name == "SETENV" && parameter) {
        const std::size_t separator = parameter->find(' ');
        if (separator != std::string_view::npos) {
            (void)environment_.set(
                std::string(parameter->substr(0, separator)),
                std::string(parameter->substr(separator + 1)));
        }
        return std::nullopt;
    }
    if (command_name == "TIMEOUT") {
        const int session = new_session();
        schedule_timeout(parse_ticks(parameter), session);
        return std::to_string(session);
    }
    if (command_name == "STARTTIME") {
        return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
            start_time_.time_since_epoch()).count());
    }
    if (command_name == "STAT" && parameter) {
        if (*parameter == "mqlen") {
            return std::to_string(message_queue_.length());
        }
        if (*parameter == "message") {
            return std::to_string(message_count());
        }
        if (*parameter == "endless") {
            return "0";
        }
        if (*parameter == "cpu" || *parameter == "time") {
            return "0";
        }
        return std::string{};
    }
    if (command_name == "EXIT" || command_name == "ABORT" ||
            (command_name == "KILL" &&
             (!parameter || resolve(*parameter) == handle_))) {
        exit_requested_.store(true, std::memory_order_release);
        return std::nullopt;
    }
    if (command_name == "LOGON" || command_name == "LOGOFF" ||
            command_name == "SIGNAL" || command_name == "MONITOR") {
        return std::nullopt;
    }
    return std::nullopt;
}

bool Context::register_name(std::string_view name, Handle handle) {
    if (name_handler_) {
        return name_handler_(name, handle);
    }
    return local_names_.emplace(std::string(name), handle).second;
}

void Context::schedule_timeout(std::uint32_t ticks, int session) {
    if (timeout_handler_) {
        timeout_handler_(ticks, handle_, session);
        return;
    }
    Message response;
    response.source = 0;
    response.session = session;
    response.type = MessageType::response;
    (void)message_queue_.push(std::move(response));
}

}  // namespace skynet::runtime
