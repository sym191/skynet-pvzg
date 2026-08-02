#pragma once

#include "Environment.hpp"
#include "Message.hpp"
#include "MessageQueue.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace skynet::runtime {

// Context is the service-facing part of the runtime. The callbacks let the
// scheduler/runtime own routing and service management while keeping services
// independent from a concrete dispatcher implementation.
class Context {
public:
    using SendHandler = std::function<bool(Handle destination, Message message)>;
    using ResolveHandler = std::function<Handle(std::string_view address)>;
    using NameHandler = std::function<bool(std::string_view name, Handle handle)>;
    using CommandHandler = std::function<std::optional<std::string>(
        std::string_view command, std::optional<std::string_view> parameter)>;
    using LogHandler = std::function<void(std::string_view message)>;
    using TimeoutHandler = std::function<void(
        std::uint32_t ticks, Handle destination, int session)>;

    explicit Context(Handle handle = 0x0100'0001U);

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    [[nodiscard]] Handle handle() const noexcept;
    void set_handle(Handle handle) noexcept;

    [[nodiscard]] int new_session() noexcept;
    [[nodiscard]] int send(
        Handle destination,
        Handle source,
        int type,
        int session,
        std::span<const std::byte> payload,
        bool allocate_session = false);
    [[nodiscard]] int send(
        std::string_view destination,
        Handle source,
        int type,
        int session,
        std::span<const std::byte> payload,
        bool allocate_session = false);

    [[nodiscard]] Handle resolve(std::string_view address) const;
    [[nodiscard]] bool is_remote(Handle handle, int* harbor = nullptr) const;

    [[nodiscard]] std::optional<std::string> command(
        std::string_view command,
        std::optional<std::string_view> parameter = std::nullopt);
    void log(std::string_view message) const;

    [[nodiscard]] Environment& environment() noexcept;
    [[nodiscard]] const Environment& environment() const noexcept;
    [[nodiscard]] MessageQueue& message_queue() noexcept;
    [[nodiscard]] const MessageQueue& message_queue() const noexcept;

    void record_dispatch() noexcept;
    [[nodiscard]] std::uint64_t message_count() const noexcept;
    [[nodiscard]] bool exit_requested() const noexcept;

    void set_send_handler(SendHandler handler);
    void set_resolve_handler(ResolveHandler handler);
    void set_name_handler(NameHandler handler);
    void set_command_handler(CommandHandler handler);
    void set_log_handler(LogHandler handler);
    void set_timeout_handler(TimeoutHandler handler);

private:
    [[nodiscard]] std::optional<std::string> builtin_command(
        std::string_view command,
        std::optional<std::string_view> parameter);
    [[nodiscard]] bool register_name(std::string_view name, Handle handle);
    void schedule_timeout(std::uint32_t ticks, int session);

    Handle handle_;
    std::atomic<int> next_session_{1};
    std::atomic<std::uint64_t> message_count_{0};
    std::atomic<bool> exit_requested_{false};
    const std::chrono::system_clock::time_point start_time_;

    Environment environment_;
    MessageQueue message_queue_;
    std::unordered_map<std::string, Handle> local_names_;

    SendHandler send_handler_;
    ResolveHandler resolve_handler_;
    NameHandler name_handler_;
    CommandHandler command_handler_;
    LogHandler log_handler_;
    TimeoutHandler timeout_handler_;
};

}  // namespace skynet::runtime
