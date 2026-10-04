#include "Context.hpp"

#include "Runtime.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace skynet::runtime {
namespace {

[[nodiscard]] std::int64_t SteadyNanoseconds(
    std::chrono::steady_clock::time_point time
) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        time.time_since_epoch()).count();
}

}  // namespace

Context::Context(Runtime& runtime, ServicePtr service)
    : runtime_(runtime), service_(std::move(service)) {}

Handle Context::handle() const noexcept {
    return handle_;
}

void Context::SetHandle(Handle handle) noexcept {
    handle_ = handle;
    queue_.set_owner(handle);
}

bool Context::Initialize(std::string_view args) {
    if (service_ == nullptr || initialized_.exchange(true)) {
        return false;
    }
    try {
        if (!service_->init(*this, args)) {
            accepting_.store(false, std::memory_order_release);
            initialized_.store(false, std::memory_order_release);
            return false;
        }
    } catch (const std::exception& error) {
        Log(std::string("service initialization threw: ") + error.what());
        accepting_.store(false, std::memory_order_release);
        initialized_.store(false, std::memory_order_release);
        return false;
    } catch (...) {
        Log("service initialization threw an unknown exception");
        accepting_.store(false, std::memory_order_release);
        initialized_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

bool Context::Activate() noexcept {
    std::lock_guard lock(lifecycle_mutex_);
    if (!initialized_.load(std::memory_order_acquire) ||
        !accepting_.load(std::memory_order_acquire)) {
        return false;
    }
    active_.store(true, std::memory_order_release);
    return queue_.length() != 0;
}

std::vector<Message> Context::Deactivate() {
    std::lock_guard lock(lifecycle_mutex_);
    accepting_.store(false, std::memory_order_release);
    active_.store(false, std::memory_order_release);
    return queue_.drain();
}

bool Context::active() const noexcept {
    return active_.load(std::memory_order_acquire);
}

EnqueueResult Context::Enqueue(Message message) {
    std::lock_guard lock(lifecycle_mutex_);
    if (!accepting_.load(std::memory_order_acquire)) {
        return EnqueueResult::rejected;
    }
    const bool schedule = queue_.push(std::move(message));
    if (!active_.load(std::memory_order_acquire)) {
        return EnqueueResult::queued;
    }
    return schedule ? EnqueueResult::schedule : EnqueueResult::queued;
}

void Context::DispatchOne() noexcept {
    if (!active_.load(std::memory_order_acquire)) {
        return;
    }
    auto message = queue_.pop();
    if (!message) {
        return;
    }

    const auto started = std::chrono::steady_clock::now();
    dispatch_started_ns_.store(SteadyNanoseconds(started), std::memory_order_release);
    dispatching_.store(true, std::memory_order_release);
    message_count_.fetch_add(1, std::memory_order_relaxed);

    if (const std::size_t overload = queue_.take_overload(); overload != 0) {
        Log("message queue overload: " + std::to_string(overload));
    }

    if (message_logging_.load(std::memory_order_relaxed)) {
        Log("dispatch type=" + std::to_string(message->type) +
            " source=" + std::to_string(message->source) +
            " session=" + std::to_string(message->session));
    }

    try {
        service_->dispatch(*this, std::move(*message));
    } catch (const std::exception& error) {
        Log(std::string("service dispatch threw: ") + error.what());
    } catch (...) {
        Log("service dispatch threw an unknown exception");
    }

    if (runtime_.profile_enabled()) {
        const auto elapsed = std::chrono::steady_clock::now() - started;
        cpu_microseconds_.fetch_add(
            static_cast<std::uint64_t>(std::chrono::duration_cast<
                std::chrono::microseconds>(elapsed).count()),
            std::memory_order_relaxed);
    }
    dispatching_.store(false, std::memory_order_release);
    dispatch_started_ns_.store(0, std::memory_order_release);
}

bool Context::RescheduleAfterDispatch() {
    if (!active_.load(std::memory_order_acquire)) {
        queue_.clear();
        return false;
    }
    return queue_.reschedule_after_dispatch();
}

int Context::NewSession() noexcept {
    int current = next_session_.load(std::memory_order_relaxed);
    while (true) {
        const int result = current >= kMaximumSession ? 1 : current + 1;
        if (next_session_.compare_exchange_weak(
                current, result, std::memory_order_relaxed)) {
            return result;
        }
    }
}

int Context::Send(
    Handle destination,
    ProtocolType type,
    int session,
    std::span<const std::byte> payload,
    Handle source,
    bool allocate_session
) {
    if (allocate_session) {
        session = NewSession();
    }
    if (destination == 0) {
        return payload.empty() ? session : -1;
    }

    Message message{
        .source = source == 0 ? handle_ : source,
        .session = session,
        .type = type,
        .data = MessageBuffer(payload.begin(), payload.end()),
    };
    return runtime_.Send(destination, std::move(message)) ? session : -1;
}

int Context::Send(
    std::string_view destination,
    ProtocolType type,
    int session,
    std::span<const std::byte> payload,
    Handle source,
    bool allocate_session
) {
    const Handle handle = runtime_.Resolve(destination);
    return Send(handle, type, session, payload, source, allocate_session);
}

std::optional<std::string> Context::Command(
    std::string_view command,
    std::optional<std::string_view> parameter
) {
    return runtime_.Command(*this, command, parameter);
}

std::uint64_t Context::NowTicks() const noexcept {
    return runtime_.NowTicks();
}

bool Context::IsRemote(Handle handle, int* harbor) const noexcept {
    const int destination_harbor = static_cast<int>(handle >> kHandleRemoteShift);
    if (harbor != nullptr) {
        *harbor = destination_harbor;
    }
    const int local_harbor = static_cast<int>(handle_ >> kHandleRemoteShift);
    return destination_harbor != 0 && destination_harbor != local_harbor;
}

void Context::Log(std::string_view message) const {
    runtime_.Log(handle_, message);
}

void Context::Signal(int signal) noexcept {
    std::lock_guard lock(signal_mutex_);
    try {
        service_->signal(*this, signal);
    } catch (...) {
        Log("service signal threw an exception");
    }
}

std::size_t Context::queue_length() const {
    return queue_.length();
}

std::uint64_t Context::message_count() const noexcept {
    return message_count_.load(std::memory_order_relaxed);
}

double Context::cpu_seconds() const noexcept {
    return static_cast<double>(cpu_microseconds_.load(std::memory_order_relaxed)) /
        1'000'000.0;
}

double Context::current_dispatch_seconds() const noexcept {
    const auto started_ns = dispatch_started_ns_.load(std::memory_order_acquire);
    if (started_ns == 0) {
        return 0.0;
    }
    const auto now_ns = SteadyNanoseconds(std::chrono::steady_clock::now());
    return static_cast<double>(std::max<std::int64_t>(0, now_ns - started_ns)) /
        1'000'000'000.0;
}

bool Context::ConsumeEndless() noexcept {
    return endless_.exchange(false, std::memory_order_acq_rel);
}

void Context::CheckEndless(std::chrono::steady_clock::time_point now) noexcept {
    if (!dispatching_.load(std::memory_order_acquire)) {
        return;
    }
    const auto started_ns = dispatch_started_ns_.load(std::memory_order_acquire);
    if (started_ns == 0) {
        return;
    }
    constexpr auto kEndlessThreshold = std::chrono::seconds(5);
    if (SteadyNanoseconds(now) - started_ns >= kEndlessThreshold.count() *
            1'000'000'000LL) {
        endless_.store(true, std::memory_order_release);
    }
}

void Context::set_message_logging(bool enabled) noexcept {
    message_logging_.store(enabled, std::memory_order_release);
}

bool Context::message_logging() const noexcept {
    return message_logging_.load(std::memory_order_acquire);
}

}  // namespace skynet::runtime
