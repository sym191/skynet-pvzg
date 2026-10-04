#pragma once

#include "Message.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace skynet::runtime {

class TimerService final {
public:
    using Callback = std::function<void(Handle, int)>;

    TimerService() = default;
    ~TimerService();

    TimerService(const TimerService&) = delete;
    TimerService& operator=(const TimerService&) = delete;

    [[nodiscard]] bool Start(Callback callback);
    void Schedule(std::uint32_t ticks, Handle destination, int session);
    void RequestStop() noexcept;
    void Join() noexcept;

    [[nodiscard]] std::uint64_t NowTicks() const noexcept;
    [[nodiscard]] std::uint32_t StartTime() const noexcept;

private:
    struct Event {
        std::chrono::steady_clock::time_point deadline;
        std::uint64_t sequence;
        Handle destination;
        int session;
    };

    struct LaterEvent {
        bool operator()(const Event& left, const Event& right) const noexcept;
    };

    void Run(std::stop_token stop_token) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    std::priority_queue<Event, std::vector<Event>, LaterEvent> events_;
    Callback callback_;
    std::jthread worker_;
    std::chrono::steady_clock::time_point steady_start_{};
    std::uint32_t wall_start_{0};
    std::uint32_t initial_ticks_{0};
    std::uint64_t next_sequence_{0};
    bool running_{false};
};

}  // namespace skynet::runtime
