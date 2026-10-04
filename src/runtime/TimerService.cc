#include "TimerService.hpp"

#include <ctime>
#include <utility>

namespace skynet::runtime {

namespace {

constexpr auto kTickDuration = std::chrono::milliseconds(10);

}  // namespace

TimerService::~TimerService() {
    RequestStop();
    Join();
}

bool TimerService::LaterEvent::operator()(
    const Event& left,
    const Event& right
) const noexcept {
    if (left.deadline != right.deadline) {
        return left.deadline > right.deadline;
    }
    return left.sequence > right.sequence;
}

bool TimerService::Start(Callback callback) {
    std::lock_guard lock(mutex_);
    if (running_ || !callback) {
        return running_;
    }
    callback_ = std::move(callback);
    steady_start_ = std::chrono::steady_clock::now();
    const auto wall_now = std::chrono::system_clock::now();
    wall_start_ = static_cast<std::uint32_t>(
        std::chrono::system_clock::to_time_t(wall_now));
    initial_ticks_ = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            wall_now.time_since_epoch()).count() % 1000 / 10);
    running_ = true;
    try {
        worker_ = std::jthread(
            [this](std::stop_token token) { Run(token); });
    } catch (...) {
        running_ = false;
        callback_ = {};
        return false;
    }
    return true;
}

void TimerService::Schedule(
    std::uint32_t ticks,
    Handle destination,
    int session
) {
    Callback immediate;
    {
        std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        if (ticks == 0) {
            immediate = callback_;
        } else {
            events_.push(Event{
                .deadline = std::chrono::steady_clock::now() +
                    kTickDuration * ticks,
                .sequence = next_sequence_++,
                .destination = destination,
                .session = session,
            });
            ready_.notify_all();
            return;
        }
    }
    if (immediate) {
        immediate(destination, session);
    }
}

void TimerService::RequestStop() noexcept {
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        if (worker_.joinable()) {
            worker_.request_stop();
        }
    }
    ready_.notify_all();
}

void TimerService::Join() noexcept {
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard lock(mutex_);
    events_ = {};
    callback_ = {};
}

std::uint64_t TimerService::NowTicks() const noexcept {
    std::lock_guard lock(mutex_);
    if (steady_start_ == std::chrono::steady_clock::time_point{}) {
        return 0;
    }
    return initial_ticks_ + static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - steady_start_).count() / 10);
}

std::uint32_t TimerService::StartTime() const noexcept {
    std::lock_guard lock(mutex_);
    return wall_start_;
}

void TimerService::Run(std::stop_token stop_token) noexcept {
    std::unique_lock lock(mutex_);
    while (!stop_token.stop_requested()) {
        if (events_.empty()) {
            ready_.wait(lock, stop_token, [this] {
                return !events_.empty() || !running_;
            });
            continue;
        }

        const auto deadline = events_.top().deadline;
        ready_.wait_until(lock, stop_token, deadline, [this, deadline] {
            return !running_ || events_.empty() ||
                events_.top().deadline < deadline;
        });
        if (stop_token.stop_requested() || !running_) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        while (!events_.empty() && events_.top().deadline <= now) {
            const Event event = events_.top();
            events_.pop();
            const Callback callback = callback_;
            lock.unlock();
            if (callback) {
                callback(event.destination, event.session);
            }
            lock.lock();
        }
    }
}

}  // namespace skynet::runtime
