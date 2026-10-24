#include "Context.hpp"

#include "Runtime.hpp"
#include "SocketEvent.hpp"
#include "ThreadClock.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

namespace skynet::runtime {
namespace {
thread_local Context* current_context = nullptr;
struct CurrentGuard {
    Context* previous;
    explicit CurrentGuard(Context* context) : previous(std::exchange(current_context, context)) {}
    ~CurrentGuard() { current_context = previous; }
};

}  // namespace

Context::Context(Runtime& runtime, ServicePtr service)
    : runtime_(runtime), service_(std::move(service)) {}

Context::~Context() {
    CurrentGuard guard(this);
    service_.reset();
}

Context* Context::current() noexcept { return current_context; }

void Context::set_callback(void* userdata, NativeCallback callback) noexcept {
    callback_ = callback;
    callback_userdata_ = userdata;
}

void Context::dispatch_native(Message message) {
    if (invoke_native(message)) {
        (void)message.data.release();
    }
}

bool Context::invoke_native(Message& message) {
    if (!callback_) { return false; }
    // The native consumer now owns the nested buffer, independently of whether
    // it retains the outer message. Rejected/unconsumed messages keep RAII cleanup.
    handoff_socket_message(message.data);
    return callback_(reinterpret_cast<skynet_context*>(this), callback_userdata_,
        message.type, message.session, message.source, message.data.data(), message.data.size());
}

const char* Context::native_command(const char* command, const char* parameter) {
    command_result_ = this->command(command, parameter ? std::optional<std::string_view>(parameter) : std::nullopt);
    return command_result_ ? command_result_->c_str() : nullptr;
}

Handle Context::handle() const noexcept {
    return handle_;
}

void Context::set_handle(Handle handle) noexcept {
    handle_ = handle;
    queue_.set_owner(handle);
}

bool Context::initialize(std::string_view args) {
    CurrentGuard guard(this);
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
        log(std::string("service initialization threw: ") + error.what());
        accepting_.store(false, std::memory_order_release);
        initialized_.store(false, std::memory_order_release);
        return false;
    } catch (...) {
        log("service initialization threw an unknown exception");
        accepting_.store(false, std::memory_order_release);
        initialized_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

bool Context::activate() noexcept {
    std::lock_guard lock(lifecycle_mutex_);
    if (!initialized_.load(std::memory_order_acquire) ||
        !accepting_.load(std::memory_order_acquire)) {
        return false;
    }
    active_.store(true, std::memory_order_release);
    return queue_.length() != 0;
}

std::vector<Message> Context::deactivate() {
    std::lock_guard lock(lifecycle_mutex_);
    accepting_.store(false, std::memory_order_release);
    active_.store(false, std::memory_order_release);
    return queue_.drain();
}

bool Context::active() const noexcept {
    return active_.load(std::memory_order_acquire);
}

EnqueueResult Context::enqueue(Message message) {
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

void Context::dispatch_one(std::size_t worker_index) noexcept {
    CurrentGuard guard(this);
    if (!active_.load(std::memory_order_acquire)) {
        return;
    }
    auto message = queue_.pop();
    if (!message) {
        return;
    }

    const auto cpu_started = thread_microseconds();
    dispatch_cpu_started_us_.store(cpu_started, std::memory_order_release);
    message_count_.fetch_add(1, std::memory_order_relaxed);

    if (const std::size_t overload = queue_.take_overload(); overload != 0) {
        log("message queue overload: " + std::to_string(overload));
    }

    {
        std::lock_guard lock(log_mutex_);
        if (message_log_.is_open()) {
            message_log_ << ':' << std::hex << std::setw(8) << std::setfill('0') << message->source
                         << std::dec << ' ' << unsigned(message->type) << ' ' << message->session
                         << ' ' << now_ticks() << ' ';
            for (auto byte : message->data) {
                message_log_ << std::hex << std::setw(2) << std::setfill('0') << std::to_integer<unsigned>(byte);
            }
            message_log_ << std::dec << '\n';
            message_log_.flush();
        }
    }

    runtime_.trigger(worker_index, message->source, handle_);
    try {
        service_->dispatch(*this, std::move(*message));
    } catch (const std::exception& error) {
        log(std::string("service dispatch threw: ") + error.what());
    } catch (...) {
        log("service dispatch threw an unknown exception");
    }
    runtime_.trigger(worker_index, 0, 0);

    if (runtime_.profile_enabled()) {
        cpu_microseconds_.fetch_add(thread_microseconds() - cpu_started, std::memory_order_relaxed);
    }
    dispatch_cpu_started_us_.store(0, std::memory_order_release);
}

bool Context::reschedule_after_dispatch() {
    if (!active_.load(std::memory_order_acquire)) {
        queue_.clear();
        return false;
    }
    return queue_.reschedule_after_dispatch();
}

int Context::new_session() noexcept {
    int current = next_session_.load(std::memory_order_relaxed);
    while (true) {
        const int result = current >= kMaximumSession ? 1 : current + 1;
        if (next_session_.compare_exchange_weak(
                current, result, std::memory_order_relaxed)) {
            return result;
        }
    }
}

int Context::send(
    Handle destination,
    ProtocolType type,
    int session,
    std::span<const std::byte> payload,
    Handle source,
    bool allocate_session
) {
    if (allocate_session) {
        session = new_session();
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
    return runtime_.send(destination, std::move(message)) ? session : -1;
}

int Context::send(
    std::string_view destination,
    ProtocolType type,
    int session,
    std::span<const std::byte> payload,
    Handle source,
    bool allocate_session
) {
    if (!destination.empty() && destination.front() != '.' && destination.front() != ':') {
        if (allocate_session) { session = new_session(); }
        Message message{.source = source == 0 ? handle_ : source, .session = session,
            .type = type, .data = MessageBuffer(payload.begin(), payload.end())};
        return runtime_.send_global(destination, std::move(message)) ? session : -1;
    }
    const Handle handle = runtime_.resolve(destination);
    if (handle == 0) { return -1; }
    return send(handle, type, session, payload, source, allocate_session);
}

std::optional<std::string> Context::command(
    std::string_view command,
    std::optional<std::string_view> parameter
) {
    return runtime_.command(*this, command, parameter);
}

std::uint64_t Context::now_ticks() const noexcept {
    return runtime_.now_ticks();
}

bool Context::is_remote(Handle handle, int* harbor) const noexcept {
    const int destination_harbor = static_cast<int>(handle >> kHandleRemoteShift);
    if (harbor != nullptr) {
        *harbor = destination_harbor;
    }
    const int local_harbor = static_cast<int>(handle_ >> kHandleRemoteShift);
    return destination_harbor != 0 && destination_harbor != local_harbor;
}

void Context::log(std::string_view message) const {
    runtime_.log(handle_, message);
}

void Context::signal(int signal) noexcept {
    std::lock_guard lock(signal_mutex_);
    try {
        service_->signal(*this, signal);
    } catch (...) {
        log("service signal threw an exception");
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
    const auto started = dispatch_cpu_started_us_.load(std::memory_order_acquire);
    if (!runtime_.profile_enabled() || started == 0 || current() != this) {
        return 0.0;
    }
    return static_cast<double>(thread_microseconds() - started) / 1'000'000.0;
}

bool Context::consume_endless() noexcept {
    return endless_.exchange(false, std::memory_order_acq_rel);
}

void Context::mark_endless() noexcept {
    endless_.store(true, std::memory_order_release);
}

void Context::set_message_logging(bool enabled) noexcept {
    try {
        std::lock_guard lock(log_mutex_);
        if (!enabled) {
            if (message_log_.is_open()) { message_log_ << "close time: " << now_ticks() << '\n'; }
            message_log_.close();
        } else if (!message_log_.is_open()) {
            if (auto path = runtime_.environment().get("logpath")) {
                std::ostringstream filename;
                filename << std::hex << std::setw(8) << std::setfill('0') << handle_ << ".log";
                message_log_.clear();
                message_log_.open(std::filesystem::path(*path) / filename.str(), std::ios::app);
                if (message_log_) { message_log_ << "open time: " << now_ticks() << '\n'; }
            }
        }
        message_logging_.store(message_log_.is_open(), std::memory_order_release);
    } catch (...) { log("failed to open service message log"); }
}

bool Context::message_logging() const noexcept {
    return message_logging_.load(std::memory_order_acquire);
}

}  // namespace skynet::runtime
