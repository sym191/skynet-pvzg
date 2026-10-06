#pragma once

#include "Message.hpp"
#include "MessageQueue.hpp"
#include "Service.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct skynet_context;

namespace skynet::runtime {

class Runtime;

enum class EnqueueResult : std::uint8_t {
    rejected,
    queued,
    schedule,
};

class Context final {
public:
    Context(Runtime& runtime, ServicePtr service);
    ~Context();
    using NativeCallback = int (*)(skynet_context*, void*, int, int, Handle, const void*, std::size_t);
    void set_callback(void* userdata, NativeCallback callback) noexcept;
    void dispatch_native(Message message);
    bool invoke_native(const Message& message);
    Runtime& runtime() noexcept { return runtime_; }
    static Context* current() noexcept;
    const char* native_command(const char* command, const char* parameter);

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    [[nodiscard]] Handle handle() const noexcept;
    void set_handle(Handle handle) noexcept;

    [[nodiscard]] bool initialize(std::string_view args);
    [[nodiscard]] bool activate() noexcept;
    [[nodiscard]] std::vector<Message> deactivate();
    [[nodiscard]] bool active() const noexcept;

    [[nodiscard]] EnqueueResult enqueue(Message message);
    void dispatch_one() noexcept;
    [[nodiscard]] bool reschedule_after_dispatch();

    [[nodiscard]] int new_session() noexcept;
    [[nodiscard]] int send(
        Handle destination,
        ProtocolType type,
        int session,
        std::span<const std::byte> payload,
        Handle source = 0,
        bool allocate_session = false
    );
    [[nodiscard]] int send(
        std::string_view destination,
        ProtocolType type,
        int session,
        std::span<const std::byte> payload,
        Handle source = 0,
        bool allocate_session = false
    );

    [[nodiscard]] std::optional<std::string> command(
        std::string_view command,
        std::optional<std::string_view> parameter = std::nullopt
    );
    [[nodiscard]] std::uint64_t now_ticks() const noexcept;
    [[nodiscard]] bool is_remote(Handle handle, int* harbor = nullptr) const noexcept;
    void log(std::string_view message) const;
    void signal(int signal) noexcept;

    [[nodiscard]] std::size_t queue_length() const;
    [[nodiscard]] std::uint64_t message_count() const noexcept;
    [[nodiscard]] double cpu_seconds() const noexcept;
    [[nodiscard]] double current_dispatch_seconds() const noexcept;
    [[nodiscard]] bool consume_endless() noexcept;
    void check_endless(std::chrono::steady_clock::time_point now) noexcept;

    void set_message_logging(bool enabled) noexcept;
    [[nodiscard]] bool message_logging() const noexcept;

private:
    friend struct RuntimeTestAccess;
    static constexpr int kMaximumSession = 0x7fff'ffff;

    Runtime& runtime_;
    ServicePtr service_;
    MessageQueue queue_;
    Handle handle_{0};
    std::atomic<int> next_session_{0};
    std::atomic<bool> accepting_{true};
    std::atomic<bool> active_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> dispatching_{false};
    std::atomic<bool> endless_{false};
    std::atomic<bool> message_logging_{false};
    std::atomic<std::uint64_t> message_count_{0};
    std::atomic<std::uint64_t> cpu_microseconds_{0};
    std::atomic<std::int64_t> dispatch_started_ns_{0};
    std::atomic<std::uint64_t> dispatch_cpu_started_us_{0};
    NativeCallback callback_{nullptr};
    void* callback_userdata_{nullptr};
    std::optional<std::string> command_result_;
    std::mutex log_mutex_;
    std::ofstream message_log_;
    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex signal_mutex_;
};

}  // namespace skynet::runtime
