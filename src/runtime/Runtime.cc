#include "Runtime.hpp"

#include "Context.hpp"
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

[[nodiscard]] std::string HandleAddress(Handle handle) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), ":%x", handle);
    return buffer;
}

[[nodiscard]] std::optional<Handle> ParseHandle(std::string_view address) {
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

[[nodiscard]] std::optional<std::uint32_t> ParseTicks(
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

[[nodiscard]] std::optional<CommandTarget> ParseSignalTarget(
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

[[nodiscard]] std::pair<std::string_view, std::string_view> SplitLaunch(
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
    : options_(options), handles_(options.harbor_id) {}

Runtime::~Runtime() {
    RequestStop();
    Join();
}

std::expected<void, std::error_code> Runtime::Start() {
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

    if (!scheduler_.Start(options_.worker_count)) {
        return std::unexpected(make_error_code(RuntimeErrc::scheduler_start_failed));
    }
    if (!timer_.Start([this](Handle destination, int session) {
            Message message{
                .source = 0,
                .session = session,
                .type = static_cast<ProtocolType>(MessageType::response),
                .data = {},
            };
            (void)Send(destination, std::move(message));
        })) {
        scheduler_.RequestStop();
        scheduler_.Join();
        return std::unexpected(make_error_code(RuntimeErrc::timer_start_failed));
    }

    state_.store(State::running, std::memory_order_release);
    try {
        monitor_worker_ = std::jthread(
            [this](std::stop_token token) { MonitorLoop(token); });
    } catch (...) {
        state_.store(State::stopped, std::memory_order_release);
        timer_.RequestStop();
        timer_.Join();
        scheduler_.RequestStop();
        scheduler_.Join();
        return std::unexpected(
            make_error_code(RuntimeErrc::scheduler_start_failed));
    }
    return {};
}

void Runtime::RequestStop() noexcept {
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (state_.load(std::memory_order_acquire) != State::running) {
            return;
        }
        state_.store(State::stopping, std::memory_order_release);
        timer_.RequestStop();
        if (monitor_worker_.joinable()) {
            monitor_worker_.request_stop();
        }
    }
    monitor_ready_.notify_all();

    auto contexts = handles_.retire_all();
    for (auto& context : contexts) {
        (void)context->Deactivate();
    }
    scheduler_.RequestStop();
}

void Runtime::Join() noexcept {
    RequestStop();
    timer_.Join();
    if (monitor_worker_.joinable()) {
        monitor_worker_.join();
    }
    scheduler_.Join();
    std::lock_guard lock(lifecycle_mutex_);
    state_.store(State::stopped, std::memory_order_release);
}

bool Runtime::running() const noexcept {
    return state_.load(std::memory_order_acquire) == State::running;
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

std::expected<Handle, std::error_code> Runtime::Launch(
    std::string_view service_name,
    std::string_view arguments
) {
    if (!running()) {
        return std::unexpected(make_error_code(RuntimeErrc::runtime_not_running));
    }
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
        if (state_.load(std::memory_order_acquire) != State::running) {
            return std::unexpected(
                make_error_code(RuntimeErrc::runtime_not_running));
        }
        handle = handles_.register_context(context);
    }
    if (handle == 0) {
        return std::unexpected(make_error_code(RuntimeErrc::handle_exhausted));
    }
    context->SetHandle(handle);

    if (!context->Initialize(arguments)) {
        (void)handles_.retire(handle);
        (void)context->Deactivate();
        return std::unexpected(make_error_code(RuntimeErrc::service_init_failed));
    }
    if (handles_.grab(handle) != context) {
        (void)context->Deactivate();
        return std::unexpected(make_error_code(RuntimeErrc::service_init_failed));
    }
    if (context->Activate()) {
        scheduler_.Schedule(context);
    }
    return handle;
}

bool Runtime::Retire(Handle handle) {
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
        (void)Send(monitor, std::move(exit_message));
    }

    ReportDroppedMessages(handle, context->Deactivate());
    return true;
}

void Runtime::AbortAll() {
    const auto handles = handles_.handles();
    for (const Handle handle : handles) {
        (void)Retire(handle);
    }
}

bool Runtime::Send(Handle destination, Message message) {
    if (!running()) {
        return false;
    }
    auto context = handles_.grab(destination);
    if (context == nullptr) {
        return false;
    }
    switch (context->Enqueue(std::move(message))) {
    case EnqueueResult::rejected:
        return false;
    case EnqueueResult::queued:
        return true;
    case EnqueueResult::schedule:
        scheduler_.Schedule(std::move(context));
        return true;
    }
    return false;
}

bool Runtime::Send(
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
    return Send(destination, std::move(message));
}

Handle Runtime::Resolve(std::string_view address) const {
    if (const auto parsed = ParseHandle(address)) {
        return *parsed;
    }
    if (!address.empty() && address.front() == '.') {
        return handles_.find_name(address.substr(1));
    }
    return 0;
}

bool Runtime::Name(Handle handle, std::string name) {
    if (!name.empty() && name.front() == '.') {
        name.erase(name.begin());
    }
    return !name.empty() && handles_.name_handle(handle, std::move(name));
}

std::optional<std::string> Runtime::Command(
    Context& context,
    std::string_view command,
    std::optional<std::string_view> parameter
) {
    if (command == "TIMEOUT") {
        const auto ticks = ParseTicks(parameter);
        if (!ticks) {
            return std::nullopt;
        }
        const int session = context.NewSession();
        ScheduleTimeout(*ticks, context.handle(), session);
        return std::to_string(session);
    }
    if (command == "REG") {
        if (!parameter || parameter->empty()) {
            return HandleAddress(context.handle());
        }
        return Name(context.handle(), std::string(*parameter))
            ? std::optional<std::string>(std::string(*parameter))
            : std::nullopt;
    }
    if (command == "QUERY" && parameter) {
        const Handle handle = Resolve(*parameter);
        return handle == 0 ? std::nullopt :
            std::optional<std::string>(HandleAddress(handle));
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
        const Handle handle = Resolve(parameter->substr(address_position));
        return handle != 0 && Name(handle, std::string(name))
            ? std::optional<std::string>(std::string(name))
            : std::nullopt;
    }
    if (command == "EXIT") {
        (void)Retire(context.handle());
        return std::nullopt;
    }
    if (command == "KILL" && parameter) {
        const Handle handle = Resolve(*parameter);
        if (handle != 0) {
            (void)Retire(handle);
        }
        return std::nullopt;
    }
    if (command == "LAUNCH" && parameter) {
        const auto [service_name, arguments] = SplitLaunch(*parameter);
        if (service_name.empty()) {
            return std::nullopt;
        }
        auto launched = Launch(service_name, arguments);
        return launched ? std::optional<std::string>(HandleAddress(*launched)) :
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
        return std::to_string(StartTime());
    }
    if (command == "ABORT") {
        AbortAll();
        return std::nullopt;
    }
    if (command == "MONITOR") {
        if (!parameter || parameter->empty()) {
            const Handle monitor = monitor_handle_.load(std::memory_order_acquire);
            return monitor == 0 ? std::nullopt :
                std::optional<std::string>(HandleAddress(monitor));
        }
        monitor_handle_.store(Resolve(*parameter), std::memory_order_release);
        return std::nullopt;
    }
    if (command == "STAT" && parameter) {
        if (*parameter == "mqlen") {
            return std::to_string(context.queue_length());
        }
        if (*parameter == "endless") {
            return context.ConsumeEndless() ? "1" : "0";
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
        const Handle handle = Resolve(*parameter);
        if (auto target = handles_.grab(handle)) {
            target->set_message_logging(command == "LOGON");
        }
        return std::nullopt;
    }
    if (command == "SIGNAL" && parameter) {
        const auto target = ParseSignalTarget(*parameter);
        if (target) {
            if (auto service = handles_.grab(Resolve(target->address))) {
                service->Signal(target->signal);
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

void Runtime::ScheduleTimeout(
    std::uint32_t ticks,
    Handle destination,
    int session
) {
    timer_.Schedule(ticks, destination, session);
}

std::uint64_t Runtime::NowTicks() const noexcept {
    return timer_.NowTicks();
}

std::uint32_t Runtime::StartTime() const noexcept {
    return timer_.StartTime();
}

bool Runtime::profile_enabled() const noexcept {
    return options_.profile;
}

std::size_t Runtime::service_count() const {
    return handles_.size();
}

void Runtime::Log(Handle source, std::string_view message) const {
    spdlog::info("[:{:08x}] {}", source, message);
}

void Runtime::MonitorLoop(std::stop_token stop_token) noexcept {
    std::unique_lock lock(monitor_mutex_);
    while (!stop_token.stop_requested()) {
        monitor_ready_.wait_for(lock, stop_token, std::chrono::seconds(1), [] {
            return false;
        });
        if (stop_token.stop_requested()) {
            break;
        }
        const auto handles = handles_.handles();
        const auto now = std::chrono::steady_clock::now();
        lock.unlock();
        for (const Handle handle : handles) {
            if (auto context = handles_.grab(handle)) {
                context->CheckEndless(now);
            }
        }
        lock.lock();
    }
}

void Runtime::ReportDroppedMessages(
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
        (void)Send(message.source, std::move(error));
    }
}

}  // namespace skynet::runtime
