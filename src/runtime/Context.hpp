#pragma once

#include "Message.hpp"
#include "MessageQueue.hpp"
#include "Service.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    [[nodiscard]] Handle handle() const noexcept;
    void SetHandle(Handle handle) noexcept;

    [[nodiscard]] bool Initialize(std::string_view args);
    [[nodiscard]] bool Activate() noexcept;
    [[nodiscard]] std::vector<Message> Deactivate();
    [[nodiscard]] bool active() const noexcept;

    [[nodiscard]] EnqueueResult Enqueue(Message message);
    void DispatchOne() noexcept;
    [[nodiscard]] bool RescheduleAfterDispatch();

    [[nodiscard]] int NewSession() noexcept;
    [[nodiscard]] int Send(
        Handle destination,
        ProtocolType type,
        int session,
        std::span<const std::byte> payload,
        Handle source = 0,
        bool allocate_session = false
    );
    [[nodiscard]] int Send(
        std::string_view destination,
        ProtocolType type,
        int session,
        std::span<const std::byte> payload,
        Handle source = 0,
        bool allocate_session = false
    );

    [[nodiscard]] std::optional<std::string> Command(
        std::string_view command,
        std::optional<std::string_view> parameter = std::nullopt
    );
    [[nodiscard]] std::uint64_t NowTicks() const noexcept;
    [[nodiscard]] bool IsRemote(Handle handle, int* harbor = nullptr) const noexcept;
    void Log(std::string_view message) const;
    void Signal(int signal) noexcept;

    [[nodiscard]] std::size_t queue_length() const;
    [[nodiscard]] std::uint64_t message_count() const noexcept;
    [[nodiscard]] double cpu_seconds() const noexcept;
    [[nodiscard]] double current_dispatch_seconds() const noexcept;
    [[nodiscard]] bool ConsumeEndless() noexcept;
    void CheckEndless(std::chrono::steady_clock::time_point now) noexcept;

    void set_message_logging(bool enabled) noexcept;
    [[nodiscard]] bool message_logging() const noexcept;

private:
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
    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex signal_mutex_;
};

}  // namespace skynet::runtime
