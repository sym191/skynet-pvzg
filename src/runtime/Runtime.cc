#include "Runtime.hpp"

#include "Context.hpp"
#include "SocketEvent.hpp"
#include "runtime_error.hpp"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace skynet::runtime {
namespace {

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
    const auto [position, error] = std::from_chars(
        address.data() + 1,
        address.data() + address.size(),
        handle,
        16);
    if (error != std::errc{} || position != address.data() + address.size() ||
        handle == 0) {
        return std::nullopt;
    }
    return handle;
}

[[nodiscard]] std::optional<std::uint32_t> parse_ticks(
    std::optional<std::string_view> value
) {
    if (!value) {
        return std::nullopt;
    }
    int ticks = 0;
    const auto [position, error] = std::from_chars(
        value->data(), value->data() + value->size(), ticks);
    if (error != std::errc{} || position != value->data() + value->size()) {
        return std::nullopt;
    }
    return ticks <= 0 ? 0U : static_cast<std::uint32_t>(ticks);
}

struct CommandTarget {
    std::string_view address;
    int signal{0};
};

[[nodiscard]] std::optional<CommandTarget> parse_signal_target(
    std::string_view parameter
) {
    const auto separator = parameter.find_first_of(" \t");
    const std::string_view address = parameter.substr(0, separator);
    int signal = 0;
    if (separator != std::string_view::npos) {
        const auto value_position = parameter.find_first_not_of(" \t", separator);
        if (value_position != std::string_view::npos) {
            const std::string value(parameter.substr(value_position));
            char* end = nullptr;
            errno = 0;
            const long parsed = std::strtol(value.c_str(), &end, 0);
            if (errno == ERANGE || end == value.c_str() || *end != '\0' ||
                parsed < std::numeric_limits<int>::min() ||
                parsed > std::numeric_limits<int>::max()) {
                return std::nullopt;
            }
            signal = static_cast<int>(parsed);
        }
    }
    return CommandTarget{.address = address, .signal = signal};
}

[[nodiscard]] std::pair<std::string_view, std::string_view> split_launch(
    std::string_view parameter
) {
    const auto begin = parameter.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    parameter.remove_prefix(begin);
    const auto separator = parameter.find_first_of(" \t\r\n");
    if (separator == std::string_view::npos) {
        return {parameter, {}};
    }
    const auto arguments = parameter.find_first_not_of(" \t\r\n", separator);
    return {
        parameter.substr(0, separator),
        arguments == std::string_view::npos ? std::string_view{} :
            parameter.substr(arguments),
    };
}

}  // namespace

Runtime::Runtime(RuntimeOptions options)
    : network_(*this), options_(options), handles_(options.harbor_id) {}

Runtime::~Runtime() {
    request_stop();
    join();
}

Runtime::LaunchGuard::~LaunchGuard() {
    std::lock_guard lock(runtime.lifecycle_mutex_);
    --runtime.active_launches_;
    runtime.lifecycle_ready_.notify_all();
}

std::expected<void, std::error_code> Runtime::start() {
    std::lock_guard lock(lifecycle_mutex_);
    const State current = state_.load(std::memory_order_acquire);
    if (current == State::running) {
        return {};
    }
    if (current == State::stopping) {
        return std::unexpected(make_error_code(RuntimeErrc::runtime_stopping));
    }
    if (options_.worker_count == 0) {
        return std::unexpected(make_error_code(RuntimeErrc::invalid_argument));
    }

    try {
        monitors_ = std::make_unique<Monitor[]>(options_.worker_count);
    } catch (...) {
        return std::unexpected(make_error_code(RuntimeErrc::scheduler_start_failed));
    }

    abort_requested_ = false;
    abort_complete_ = false;
    failed_.store(false);
    bootstrap_handle_.store(0);
    monitor_handle_.store(0, std::memory_order_release);
    if (!scheduler_.start(options_.worker_count)) {
        return std::unexpected(make_error_code(RuntimeErrc::scheduler_start_failed));
    }
    if (!timer_.start([this](Handle destination, int session) {
            Message message{
                .source = 0,
                .session = session,
                .type = static_cast<ProtocolType>(MessageType::response),
                .data = {},
            };
            (void)send(destination, std::move(message));
        })) {
        scheduler_.request_stop();
        scheduler_.join();
        return std::unexpected(make_error_code(RuntimeErrc::timer_start_failed));
    }

    if (!network_.start()) {
        timer_.request_stop();
        timer_.join();
        scheduler_.request_stop();
        scheduler_.join();
        return std::unexpected(make_error_code(RuntimeErrc::network_start_failed));
    }
    state_.store(State::running, std::memory_order_release);
    try {
        monitor_worker_ = std::jthread(
            [this](std::stop_token token) { monitor_loop(token); });
    } catch (...) {
        state_.store(State::stopped, std::memory_order_release);
        network_.join();
        timer_.request_stop();
        timer_.join();
        scheduler_.request_stop();
        scheduler_.join();
        return std::unexpected(
            make_error_code(RuntimeErrc::scheduler_start_failed));
    }
    return {};
}

void Runtime::request_stop() noexcept {
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (state_.load(std::memory_order_acquire) != State::running) {
            return;
        }
        state_.store(State::stopping, std::memory_order_release);
        stop_in_progress_ = true;
        network_.request_stop();
        timer_.request_stop();
        if (monitor_worker_.joinable()) {
            monitor_worker_.request_stop();
        }
    }
    monitor_ready_.notify_all();
    lifecycle_ready_.notify_all();

    auto contexts = handles_.retire_all();
    for (auto& context : contexts) {
        (void)context->deactivate();
        context->signal(0);
    }
    scheduler_.request_stop();
    {
        std::lock_guard lock(lifecycle_mutex_);
        stop_in_progress_ = false;
        lifecycle_ready_.notify_all();
    }
}

void Runtime::join() noexcept {
    request_stop();
    {
        std::unique_lock lock(lifecycle_mutex_);
        lifecycle_ready_.wait(lock, [this] { return !stop_in_progress_ && active_launches_ == 0; });
    }
    network_.join();
    timer_.join();
    if (monitor_worker_.joinable()) {
        monitor_worker_.join();
    }
    scheduler_.join();
    // Retained services outlive every callback, including those retired by ABORT.
    decltype(reserved_) reserved;
    {
        std::lock_guard lock(lifecycle_mutex_);
        reserved.swap(reserved_);
    }
    reserved.clear();
    {
        std::lock_guard lock(global_mutex_);
        pending_global_.clear();
        global_names_.clear();
    }
    std::lock_guard lock(lifecycle_mutex_);
    state_.store(State::stopped, std::memory_order_release);
}

bool Runtime::running() const noexcept {
    return state_.load(std::memory_order_acquire) == State::running;
}

void Runtime::wait() {
    {
        std::unique_lock lock(lifecycle_mutex_);
        lifecycle_ready_.wait(lock, [this] {
            return abort_complete_ || !running() || service_count_locked() == 0;
        });
    }
    const auto logger = handles_.grab(resolve(".logger"));
    if (logger && running()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (logger->queue_length() != 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    request_stop();
    join();
}

ServiceRegistry& Runtime::services() noexcept {
    return services_;
}

Environment& Runtime::environment() noexcept {
    return environment_;
}

const Environment& Runtime::environment() const noexcept {
    return environment_;
}

std::expected<Handle, std::error_code> Runtime::launch(
    std::string_view service_name,
    std::string_view arguments,
    bool bootstrap
) {
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (!running() || abort_requested_) {
            return std::unexpected(make_error_code(RuntimeErrc::runtime_not_running));
        }
        ++active_launches_;
    }
    LaunchGuard launch_guard{*this};
    ServicePtr service;
    try {
        service = services_.create(service_name);
    } catch (...) {
        return std::unexpected(make_error_code(RuntimeErrc::service_init_failed));
    }
    if (service == nullptr) {
        return std::unexpected(make_error_code(RuntimeErrc::service_not_found));
    }

    auto context = std::make_shared<Context>(*this, std::move(service));
    Handle handle = 0;
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (state_.load(std::memory_order_acquire) != State::running || abort_requested_) {
            return std::unexpected(
                make_error_code(RuntimeErrc::runtime_not_running));
        }
        handle = handles_.register_context(context);
    }
    if (handle == 0) {
        return std::unexpected(make_error_code(RuntimeErrc::handle_exhausted));
    }
    if (!context->initialize(arguments)) {
        (void)handles_.retire(handle);
        report_dropped_messages(handle, context->deactivate());
        network_.close_all(handle);
        return std::unexpected(make_error_code(RuntimeErrc::service_init_failed));
    }
    if (handles_.grab(handle) != context) {
        (void)context->deactivate();
        return std::unexpected(make_error_code(RuntimeErrc::service_init_failed));
    }
    if (bootstrap) { bootstrap_handle_.store(handle, std::memory_order_release); }
    if (context->activate()) {
        scheduler_.schedule(context);
    }
    return handle;
}

bool Runtime::retire(Handle handle) {
    auto context = handles_.grab(handle);
    if (context == nullptr || !handles_.retire(handle)) {
        return false;
    }

    const Handle monitor = monitor_handle_.load(std::memory_order_acquire);
    if (monitor != 0 && monitor != handle) {
        Message exit_message{
            .source = handle,
            .session = 0,
            .type = static_cast<ProtocolType>(MessageType::client),
            .data = {},
        };
        (void)send(monitor, std::move(exit_message));
    }

    report_dropped_messages(handle, context->deactivate());
    network_.close_all(handle);
    if (Context::current() != context.get()) { context->signal(0); }
    {
        std::lock_guard lock(lifecycle_mutex_);
        lifecycle_ready_.notify_all();
    }
    return true;
}

bool Runtime::reserve(Handle handle) {
    std::lock_guard lock(lifecycle_mutex_);
    auto context = handles_.grab(handle);
    return context && reserved_.emplace(handle, std::move(context)).second;
}

void Runtime::abort_all() {
    {
        std::lock_guard lock(lifecycle_mutex_);
        abort_requested_ = true;
    }
    const Handle logger = resolve(".logger");
    const auto handles = handles_.handles();
    for (const Handle handle : handles) {
        if (handle != logger) { (void)retire(handle); }
    }
    {
        std::lock_guard lock(lifecycle_mutex_);
        abort_complete_ = true;
        lifecycle_ready_.notify_all();
    }
}

void Runtime::initialization_failed(Handle handle) {
    if (bootstrap_handle_.load(std::memory_order_acquire) == handle) {
        failed_.store(true);
        abort_all();
    }
}

bool Runtime::owner_alive(network::ServiceId owner) const noexcept {
    return handles_.contains(owner);
}

bool Runtime::publish(network::Event&& event) noexcept {
    try {
        const auto destination = event.destination;
        Message message{.source = 0, .session = 0,
            .type = static_cast<ProtocolType>(MessageType::socket),
            .data = make_socket_message(std::move(event))};
        return send(destination, std::move(message));
    } catch (...) { return false; }
}

bool Runtime::send(Handle destination, Message message) {
    if (!running()) {
        return false;
    }
    // A producer (including an I/O worker) must never become the last owner of
    // a retired Context: its Lua VM can run finalizers during destruction.
    return handles_.with_context(destination, [&](const auto& context) {
        switch (context->enqueue(std::move(message))) {
        case EnqueueResult::rejected: return false;
        case EnqueueResult::queued: return true;
        case EnqueueResult::schedule:
            scheduler_.schedule(context);
            return true;
        }
        return false;
    });
}

bool Runtime::send(
    Handle destination,
    Handle source,
    ProtocolType type,
    int session,
    std::span<const std::byte> payload
) {
    Message message{
        .source = source,
        .session = session,
        .type = type,
        .data = MessageBuffer(payload.begin(), payload.end()),
    };
    return send(destination, std::move(message));
}

Handle Runtime::resolve(std::string_view address) const {
    if (const auto parsed = parse_handle(address)) {
        return *parsed;
    }
    if (!address.empty() && address.front() == '.') {
        return handles_.find_name(address.substr(1));
    }
    return 0;
}

bool Runtime::name(Handle handle, std::string name) {
    if (!name.empty() && name.front() == '.') {
        name.erase(name.begin());
    }
    return !name.empty() && handles_.name_handle(handle, std::move(name));
}

bool Runtime::send_global(std::string_view name, Message message) {
    if (name.empty() || name.size() >= 16) { return false; }
    std::lock_guard lock(global_mutex_);
    if (!running()) { return false; }
    auto found = global_names_.find(std::string(name));
    if (found != global_names_.end()) { return send(found->second, std::move(message)); }
    pending_global_[std::string(name)].push_back(std::move(message));
    return true;
}

void Runtime::register_global(std::string name, Handle handle) {
    std::lock_guard lock(global_mutex_);
    global_names_.insert_or_assign(name, handle);
    if (auto found = pending_global_.find(name); found != pending_global_.end()) {
        for (auto& message : found->second) { (void)send(handle, std::move(message)); }
        pending_global_.erase(found);
    }
}

std::optional<std::string> Runtime::command(
    Context& context,
    std::string_view command,
    std::optional<std::string_view> parameter
) {
    if (command == "TIMEOUT") {
        const auto ticks = parse_ticks(parameter);
        if (!ticks) {
            return std::nullopt;
        }
        const int session = context.new_session();
        schedule_timeout(*ticks, context.handle(), session);
        return std::to_string(session);
    }
    if (command == "REG") {
        if (!parameter || parameter->empty()) {
            return handle_address(context.handle());
        }
        return parameter->starts_with('.') && name(context.handle(), std::string(*parameter))
            ? std::optional<std::string>(std::string(parameter->substr(1)))
            : std::nullopt;
    }
    if (command == "QUERY" && parameter) {
        if (!parameter->starts_with('.')) { return std::nullopt; }
        const Handle handle = resolve(*parameter);
        return handle == 0 ? std::nullopt :
            std::optional<std::string>(handle_address(handle));
    }
    if (command == "NAME" && parameter) {
        const auto separator = parameter->find_first_of(" \t");
        if (separator == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view name = parameter->substr(0, separator);
        const auto address_position = parameter->find_first_not_of(" \t", separator);
        if (address_position == std::string_view::npos) {
            return std::nullopt;
        }
        const Handle handle = parse_handle(parameter->substr(address_position)).value_or(0);
        return name.starts_with('.') && handle != 0 && this->name(handle, std::string(name))
            ? std::optional<std::string>(std::string(name.substr(1)))
            : std::nullopt;
    }
    if (command == "EXIT") {
        (void)retire(context.handle());
        return std::nullopt;
    }
    if (command == "KILL" && parameter) {
        const Handle handle = resolve(*parameter);
        if (handle != 0) {
            (void)retire(handle);
        }
        return std::nullopt;
    }
    if (command == "LAUNCH" && parameter) {
        const auto [service_name, arguments] = split_launch(*parameter);
        if (service_name.empty()) {
            return std::nullopt;
        }
        auto launched = launch(service_name, arguments);
        return launched ? std::optional<std::string>(handle_address(*launched)) :
            std::nullopt;
    }
    if (command == "GETENV" && parameter) {
        return environment_.get(*parameter);
    }
    if (command == "SETENV" && parameter) {
        const auto separator = parameter->find(' ');
        if (separator != std::string_view::npos) {
            (void)environment_.set(
                std::string(parameter->substr(0, separator)),
                std::string(parameter->substr(separator + 1)));
        }
        return std::nullopt;
    }
    if (command == "STARTTIME") {
        return std::to_string(start_time());
    }
    if (command == "ABORT") {
        abort_all();
        return std::nullopt;
    }
    if (command == "MONITOR") {
        if (!parameter || parameter->empty()) {
            const Handle monitor = monitor_handle_.load(std::memory_order_acquire);
            return monitor == 0 ? std::nullopt :
                std::optional<std::string>(handle_address(monitor));
        }
        monitor_handle_.store(resolve(*parameter), std::memory_order_release);
        return std::nullopt;
    }
    if (command == "STAT" && parameter) {
        if (*parameter == "mqlen") {
            return std::to_string(context.queue_length());
        }
        if (*parameter == "endless") {
            return context.consume_endless() ? "1" : "0";
        }
        if (*parameter == "cpu") {
            return std::to_string(context.cpu_seconds());
        }
        if (*parameter == "time") {
            return std::to_string(context.current_dispatch_seconds());
        }
        if (*parameter == "message") {
            return std::to_string(context.message_count());
        }
        return std::string{};
    }
    if ((command == "LOGON" || command == "LOGOFF") && parameter) {
        const Handle handle = resolve(*parameter);
        if (auto target = handles_.grab(handle)) {
            target->set_message_logging(command == "LOGON");
        }
        return std::nullopt;
    }
    if (command == "SIGNAL" && parameter) {
        const auto target = parse_signal_target(*parameter);
        if (target) {
            if (auto service = handles_.grab(resolve(target->address))) {
                service->signal(target->signal);
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

void Runtime::schedule_timeout(
    std::uint32_t ticks,
    Handle destination,
    int session
) {
    timer_.schedule(ticks, destination, session);
}

std::uint64_t Runtime::now_ticks() const noexcept {
    return timer_.now_ticks();
}

std::uint32_t Runtime::start_time() const noexcept {
    return timer_.start_time();
}

bool Runtime::profile_enabled() const noexcept {
    return options_.profile;
}

std::size_t Runtime::service_count() const {
    std::lock_guard lock(lifecycle_mutex_);
    return service_count_locked();
}

std::size_t Runtime::service_count_locked() const {
    std::size_t count = 0;
    for (const auto handle : handles_.handles()) {
        if (!reserved_.contains(handle)) { ++count; }
    }
    return count;
}

void Runtime::log(Handle source, std::string_view message) {
    const Handle logger = resolve(".logger");
    if (logger != 0 && logger != source && send(logger, Message::text(source, 0, message))) { return; }
    spdlog::info("[:{:08x}] {}", source, message);
}

void Runtime::trigger(std::size_t worker_index, Handle source, Handle destination) noexcept {
    auto& monitor = monitors_[worker_index];
    monitor.source = source;
    monitor.destination = destination;
    monitor.version.fetch_add(1, std::memory_order_relaxed);
}

void Runtime::check(std::size_t worker_index) {
    auto& monitor = monitors_[worker_index];
    const auto version = static_cast<std::uint32_t>(
        monitor.version.load(std::memory_order_relaxed));
    if (version != monitor.check_version) {
        monitor.check_version = version;
        return;
    }
    const auto source = monitor.source;
    const auto destination = monitor.destination;
    if (destination != 0) {
        if (auto context = handles_.grab(destination)) {
            context->mark_endless();
        }
        log(0, "error: A message from [ " + handle_address(source) + " ] to [ " +
            handle_address(destination) + " ] maybe in an endless loop (worker = " +
            std::to_string(worker_index) + ", version = " + std::to_string(version) + ")");
    }
}

void Runtime::monitor_loop(std::stop_token stop_token) noexcept {
    std::unique_lock lock(monitor_mutex_);
    while (!stop_token.stop_requested()) {
        monitor_ready_.wait_for(lock, stop_token, std::chrono::seconds(options_.worker_count), [] {
            return false;
        });
        if (stop_token.stop_requested()) {
            break;
        }
        lock.unlock();
        for (std::size_t index = 0; index < options_.worker_count; ++index) {
            check(index);
        }
        lock.lock();
    }
}

void Runtime::report_dropped_messages(
    Handle retired,
    std::vector<Message> messages
) {
    for (const Message& message : messages) {
        if (message.source == 0 ||
            message.type == static_cast<ProtocolType>(MessageType::error)) {
            continue;
        }
        Message error{
            .source = retired,
            .session = message.session,
            .type = static_cast<ProtocolType>(MessageType::error),
            .data = {},
        };
        (void)send(message.source, std::move(error));
    }
}

}  // namespace skynet::runtime
