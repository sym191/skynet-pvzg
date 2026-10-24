#include "skynet/network/runtime.hpp"

#include "detail.hpp"

#include <pthread.h>
#include <signal.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace skynet::network {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{
        .code = code,
        .message = std::move(message),
    };
}

std::expected<void, Error> validate_config(const Config& config) {
    if (config.thread_count == 0) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "network thread_count must be greater than zero"
        ));
    }
    if (config.max_sockets == 0) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "network max_sockets must be greater than zero"
        ));
    }
    if (config.udp_buffer_size < 65507) {
        return std::unexpected(make_error(ErrorCode::invalid_argument, "udp_buffer_size must hold a full IPv4 datagram"));
    }
    if (config.read_buffer_size == 0) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "network read_buffer_size must be greater than zero"
        ));
    }
    if (config.write_hard_limit == 0) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "network write_hard_limit must be greater than zero"
        ));
    }
    if (config.write_warning_bytes != 0 &&
        config.write_warning_bytes > config.write_hard_limit) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "write_warning_bytes cannot exceed write_hard_limit"
        ));
    }
    if (config.connect_timeout_ms == 0) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "connect_timeout_ms must be greater than zero"
        ));
    }
    return {};
}

std::expected<std::shared_ptr<detail::SocketBase>, Error> acquire_socket(
    detail::Core& core,
    SocketId socket
) {
    auto value = core.directory.acquire(socket);
    if (!value) {
        return std::unexpected(make_error(
            ErrorCode::invalid_socket,
            "socket id is not active"
        ));
    }
    return value;
}

}  // namespace

class Runtime::Impl final {
public:
    enum class State : std::uint8_t {
        stopped,
        running,
        stopping,
    };

    using WorkGuard =
        asio::executor_work_guard<asio::io_context::executor_type>;

    Impl(EventSink& sink, Config value)
        : config(std::move(value)),
          core(io_context, sink, config) {
    }

    [[nodiscard]] std::expected<void, Error> require_running() const {
        const auto current = state.load(std::memory_order_acquire);
        if (current == State::running) {
            return {};
        }
        if (current == State::stopping) {
            return std::unexpected(make_error(
                ErrorCode::runtime_stopping,
                "network runtime is stopping"
            ));
        }
        return std::unexpected(make_error(
            ErrorCode::runtime_not_running,
            "network runtime is not running"
        ));
    }

    Config config;
    asio::io_context io_context;
    detail::Core core;

    mutable std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_ready;
    bool stop_in_progress{false};
    std::optional<WorkGuard> work_guard;
    std::vector<std::jthread> workers;
    std::atomic<State> state{State::stopped};
};

Runtime::Runtime(EventSink& sink, Config config)
    : impl_(std::make_unique<Impl>(sink, std::move(config))) {
}

Runtime::~Runtime() {
    request_stop();
    join();
}

std::expected<void, Error> Runtime::start() {
    std::scoped_lock lock(impl_->lifecycle_mutex);

    const auto current = impl_->state.load(std::memory_order_acquire);
    if (current == Impl::State::running) {
        return {};
    }
    if (current == Impl::State::stopping) {
        return std::unexpected(make_error(
            ErrorCode::runtime_stopping,
            "network runtime is stopping"
        ));
    }

    if (auto valid = validate_config(impl_->config); !valid) {
        return valid;
    }

    impl_->io_context.restart();
    impl_->core.accepting_events.store(true, std::memory_order_release);
    impl_->work_guard.emplace(impl_->io_context.get_executor());
    impl_->state.store(Impl::State::running, std::memory_order_release);

    try {
        impl_->workers.reserve(impl_->config.thread_count);
        for (std::size_t i = 0; i < impl_->config.thread_count; ++i) {
            impl_->workers.emplace_back([this] {
                // POSIX descriptors use write(2), unlike TCP's MSG_NOSIGNAL.
                // Keep a broken borrowed pipe/socket from terminating embedders;
                // the write completion reports EPIPE on this dedicated worker.
                sigset_t signals;
                sigemptyset(&signals);
                sigaddset(&signals, SIGPIPE);
                pthread_sigmask(SIG_BLOCK, &signals, nullptr);
                // Network handlers and EventSink::publish are expected not to
                // throw across this thread boundary.
                impl_->io_context.run();
            });
        }
    } catch (const std::exception& error) {
        impl_->core.accepting_events.store(false, std::memory_order_release);
        impl_->state.store(Impl::State::stopping, std::memory_order_release);

        if (impl_->work_guard) {
            impl_->work_guard->reset();
            impl_->work_guard.reset();
        }
        impl_->io_context.stop();

        for (auto& worker : impl_->workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        impl_->workers.clear();
        impl_->io_context.restart();
        impl_->state.store(Impl::State::stopped, std::memory_order_release);

        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            std::string("failed to start network worker: ") + error.what()
        ));
    }

    return {};
}

void Runtime::request_stop() noexcept {
    std::vector<std::shared_ptr<detail::SocketBase>> sockets;

    {
        std::scoped_lock lock(impl_->lifecycle_mutex);

        const auto current = impl_->state.load(std::memory_order_acquire);
        if (current == Impl::State::stopped ||
            current == Impl::State::stopping) {
            return;
        }

        impl_->state.store(Impl::State::stopping, std::memory_order_release);

        // Runtime shutdown intentionally suppresses late socket events. Normal
        // service-level closes should use close_socket/shutdown_socket before
        // stopping the whole runtime.
        impl_->stop_in_progress = true;
        impl_->core.accepting_events.store(false, std::memory_order_release);
        sockets = impl_->core.directory.snapshot();
    }

    // Queue close operations before releasing the work guard. The io_context
    // will drain these posts and their cancellation completions.
    for (const auto& socket : sockets) {
        socket->request_close(true);
    }

    {
        std::scoped_lock lock(impl_->lifecycle_mutex);
        if (impl_->work_guard) {
            impl_->work_guard->reset();
            impl_->work_guard.reset();
        }
        impl_->stop_in_progress = false;
        impl_->lifecycle_ready.notify_all();
    }
}

void Runtime::join() noexcept {
    request_stop();

    std::vector<std::jthread> workers;
    {
        std::unique_lock lock(impl_->lifecycle_mutex);
        impl_->lifecycle_ready.wait(lock, [this] {
            return !impl_->stop_in_progress;
        });
        workers.swap(impl_->workers);
    }

    for (auto& worker : workers) {
        if (!worker.joinable()) {
            continue;
        }

        // An owner-thread API: never detach handlers still referencing impl_.
        worker.join();
    }

    {
        std::scoped_lock lock(impl_->lifecycle_mutex);
        impl_->io_context.stop();
        impl_->io_context.restart();
        impl_->state.store(Impl::State::stopped, std::memory_order_release);
    }
}

bool Runtime::running() const noexcept {
    return impl_->state.load(std::memory_order_acquire) ==
        Impl::State::running;
}

std::expected<SocketId, Error> Runtime::listen(
    ServiceId owner,
    std::string host,
    std::uint16_t port,
    int backlog
) {
    if (owner == 0) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "listen owner cannot be zero"
        ));
    }
    if (backlog <= 0) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "listen backlog must be greater than zero"
        ));
    }

    std::unique_lock lock(impl_->lifecycle_mutex);
    if (auto ready = impl_->require_running(); !ready) {
        return std::unexpected(ready.error());
    }

    if (!impl_->core.sink.owner_alive(owner)) {
        return std::unexpected(make_error(ErrorCode::wrong_owner, "socket owner has retired"));
    }
    auto reserved = impl_->core.directory.reserve();
    if (!reserved) {
        return std::unexpected(reserved.error());
    }

    const SocketId id = *reserved;
    auto socket = detail::make_listener(impl_->core, id, owner);
    if (!impl_->core.directory.install(id, socket)) {
        impl_->core.directory.erase(id);
        return std::unexpected(make_error(
            ErrorCode::invalid_socket,
            "failed to install listener in socket directory"
        ));
    }

    try {
        auto opened = detail::open_listener(socket, std::move(host), port, backlog);
        // Native Skynet listen returns -1 for bind/resolve errors. Wait for the
        // OS result, while allowing stop/retire to cancel a pending listen.
        lock.unlock();
        if (auto result = opened.get(); !result) {
            return std::unexpected(result.error());
        }
    } catch (const std::exception& error) {
        impl_->core.directory.erase(id);
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            std::string("failed to queue listen operation: ") + error.what()
        ));
    }

    return id;
}

std::expected<SocketId, Error> Runtime::connect(
    ServiceId owner,
    std::string host,
    std::uint16_t port
) {
    if (owner == 0) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "connect owner cannot be zero"
        ));
    }
    if (host.empty()) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "connect host cannot be empty"
        ));
    }
    if (port == 0) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "connect port cannot be zero"
        ));
    }

    std::scoped_lock lock(impl_->lifecycle_mutex);
    if (auto ready = impl_->require_running(); !ready) {
        return std::unexpected(ready.error());
    }

    if (!impl_->core.sink.owner_alive(owner)) {
        return std::unexpected(make_error(ErrorCode::wrong_owner, "socket owner has retired"));
    }
    auto reserved = impl_->core.directory.reserve();
    if (!reserved) {
        return std::unexpected(reserved.error());
    }

    const SocketId id = *reserved;
    auto socket = detail::make_outbound_connection(impl_->core, id, owner);
    if (!impl_->core.directory.install(id, socket)) {
        impl_->core.directory.erase(id);
        return std::unexpected(make_error(
            ErrorCode::invalid_socket,
            "failed to install outbound connection in socket directory"
        ));
    }

    try {
        detail::begin_connect(socket, std::move(host), port);
    } catch (const std::exception& error) {
        impl_->core.directory.erase(id);
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            std::string("failed to queue connect operation: ") + error.what()
        ));
    }

    return id;
}

std::expected<SocketId, Error> Runtime::udp_bind(
    ServiceId owner, std::string host, std::uint16_t port) {
    // Match upstream's synchronous resolver result without blocking lifecycle
    // operations (stop/retire/info) behind a potentially slow DNS query.
    if (!host.empty() && host != "*") {
        auto endpoint = detail::resolve_udp({std::move(host), port ? port : std::uint16_t{1}});
        if (!endpoint) { return std::unexpected(endpoint.error()); }
        host = std::move(endpoint->host);
    }
    std::scoped_lock lock(impl_->lifecycle_mutex);
    if (auto ready = impl_->require_running(); !ready) { return std::unexpected(ready.error()); }
    if (!owner || !impl_->core.sink.owner_alive(owner)) {
        return std::unexpected(make_error(ErrorCode::wrong_owner, "UDP owner is not alive"));
    }
    auto id = impl_->core.directory.reserve();
    if (!id) { return std::unexpected(id.error()); }
    try {
        auto socket = detail::make_udp_socket(impl_->core, *id, owner, std::move(host), port);
        if (!socket) {
            impl_->core.directory.erase(*id);
            return std::unexpected(socket.error());
        }
        impl_->core.directory.install(*id, *socket);
        detail::begin_udp(*socket);
    } catch (...) { impl_->core.directory.erase(*id); throw; }
    return *id;
}

std::expected<void, Error> Runtime::udp_connect(
    ServiceId, SocketId socket, Endpoint endpoint) {
    auto target = detail::resolve_udp(std::move(endpoint));
    if (!target) { return std::unexpected(target.error()); }
    std::scoped_lock lock(impl_->lifecycle_mutex);
    if (auto ready = impl_->require_running(); !ready) { return ready; }
    auto value = acquire_socket(impl_->core, socket);
    if (!value) { return std::unexpected(value.error()); }
    return (*value)->request_udp_connect(std::move(*target));
}

std::expected<void, Error> Runtime::udp_send(
    ServiceId, SocketId socket, Buffer buffer, Endpoint endpoint) {
    std::scoped_lock lock(impl_->lifecycle_mutex);
    if (auto ready = impl_->require_running(); !ready) { return ready; }
    auto value = acquire_socket(impl_->core, socket);
    if (!value) { return std::unexpected(value.error()); }
    return (*value)->request_udp_send(std::move(buffer), std::move(endpoint));
}

std::expected<SocketId, Error> Runtime::bind_fd(ServiceId owner, int fd) {
    std::scoped_lock lock(impl_->lifecycle_mutex);
    if (auto ready = impl_->require_running(); !ready) { return std::unexpected(ready.error()); }
    if (!owner || !impl_->core.sink.owner_alive(owner) || fd < 0) {
        return std::unexpected(make_error(ErrorCode::invalid_argument, "invalid FD or owner"));
    }
    auto id = impl_->core.directory.reserve();
    if (!id) { return std::unexpected(id.error()); }
    try {
        auto socket = detail::make_fd_stream(impl_->core, *id, owner, fd);
        if (!socket) {
            impl_->core.directory.erase(*id);
            return std::unexpected(socket.error());
        }
        impl_->core.directory.install(*id, *socket);
        detail::begin_fd(*socket);
    } catch (...) { impl_->core.directory.erase(*id); throw; }
    return *id;
}

std::vector<SocketInfo> Runtime::info() {
    std::vector<std::future<SocketInfo>> pending;
    {
        std::scoped_lock lock(impl_->lifecycle_mutex);
        if (!impl_->require_running()) { return {}; }
        for (const auto& socket : impl_->core.directory.snapshot()) {
            pending.push_back(socket->request_info());
        }
    }
    std::vector<SocketInfo> result;
    for (auto& future : pending) {
        auto info = future.get();
        if (info.type != SocketType::unknown) { result.push_back(std::move(info)); }
    }
    return result;
}

std::expected<void, Error> Runtime::start_socket(
    ServiceId new_owner,
    SocketId socket
) {
    if (new_owner == 0) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            "new socket owner cannot be zero"
        ));
    }
    std::scoped_lock lock(impl_->lifecycle_mutex);
    if (auto ready = impl_->require_running(); !ready) {
        return ready;
    }

    auto value = acquire_socket(impl_->core, socket);
    if (!value) {
        return std::unexpected(value.error());
    }

    (*value)->request_start(new_owner);
    return {};
}

std::expected<void, Error> Runtime::pause_socket(
    ServiceId,
    SocketId socket
) {
    std::scoped_lock lock(impl_->lifecycle_mutex);
    if (auto ready = impl_->require_running(); !ready) {
        return ready;
    }

    // These operations are intentionally available across services, as in Skynet.
    auto value = acquire_socket(impl_->core, socket);
    if (!value) {
        return std::unexpected(value.error());
    }

    (*value)->request_pause();
    return {};
}

std::expected<void, Error> Runtime::send(
    ServiceId,
    SocketId socket,
    Buffer buffer,
    SendPriority priority
) {
    std::scoped_lock lock(impl_->lifecycle_mutex);
    if (auto ready = impl_->require_running(); !ready) {
        return ready;
    }

    // These operations are intentionally available across services, as in Skynet.
    auto value = acquire_socket(impl_->core, socket);
    if (!value) {
        return std::unexpected(value.error());
    }
    return (*value)->request_send(std::move(buffer), priority);
}

std::expected<void, Error> Runtime::send(
    ServiceId owner,
    SocketId socket,
    std::span<const std::byte> data,
    SendPriority priority
) {
    try {
        Buffer buffer(data.begin(), data.end());
        return send(owner, socket, std::move(buffer), priority);
    } catch (const std::exception& error) {
        return std::unexpected(make_error(
            ErrorCode::invalid_argument,
            std::string("failed to copy send buffer: ") + error.what()
        ));
    }
}

std::expected<void, Error> Runtime::close_socket(
    ServiceId,
    SocketId socket
) {
    std::scoped_lock lock(impl_->lifecycle_mutex);
    if (auto ready = impl_->require_running(); !ready) {
        return ready;
    }

    // These operations are intentionally available across services, as in Skynet.
    auto value = acquire_socket(impl_->core, socket);
    if (!value) {
        return std::unexpected(value.error());
    }

    (*value)->request_close(false);
    return {};
}

std::expected<void, Error> Runtime::shutdown_socket(
    ServiceId,
    SocketId socket
) {
    std::scoped_lock lock(impl_->lifecycle_mutex);
    if (auto ready = impl_->require_running(); !ready) {
        return ready;
    }

    // These operations are intentionally available across services, as in Skynet.
    auto value = acquire_socket(impl_->core, socket);
    if (!value) {
        return std::unexpected(value.error());
    }

    (*value)->request_close(true);
    return {};
}

std::expected<void, Error> Runtime::set_nodelay(
    ServiceId,
    SocketId socket,
    bool enabled
) {
    std::scoped_lock lock(impl_->lifecycle_mutex);
    if (auto ready = impl_->require_running(); !ready) {
        return ready;
    }

    // These operations are intentionally available across services, as in Skynet.
    auto value = acquire_socket(impl_->core, socket);
    if (!value) {
        return std::unexpected(value.error());
    }
    if ((*value)->kind() != detail::SocketKind::tcp_connection &&
        (*value)->kind() != detail::SocketKind::fd) {
        return std::unexpected(make_error(ErrorCode::wrong_socket_kind, "nodelay requires a stream socket"));
    }

    (*value)->request_nodelay(enabled);
    return {};
}

void Runtime::close_all(ServiceId owner) noexcept {
    std::scoped_lock lock(impl_->lifecycle_mutex);
    if (!impl_->require_running()) {
        return;
    }
    const auto sockets = impl_->core.directory.snapshot();
    for (const auto& socket : sockets) {
        // Check on the strand: a queued transfer may not be visible in the
        // snapshot yet. Checking the atomic owner here can miss that transfer.
        socket->request_close(true, owner);
    }
}

Buffer copy_buffer(std::string_view text) {
    Buffer result(text.size());
    if (!text.empty()) {
        std::memcpy(result.data(), text.data(), text.size());
    }
    return result;
}

}  // namespace skynet::network
