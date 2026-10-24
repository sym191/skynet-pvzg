#pragma once

#include <atomic>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <asio.hpp>

#include "skynet/network/runtime.hpp"

namespace skynet::network::detail {

using Tcp = asio::ip::tcp;
using Udp = asio::ip::udp;
using Strand = asio::strand<asio::io_context::executor_type>;

enum class SocketKind : std::uint8_t {
    tcp_connection,
    tcp_listener,
    udp,
    fd,
};

class SocketBase;

class SocketDirectory final {
public:
    explicit SocketDirectory(std::size_t max_sockets);

    [[nodiscard]] std::expected<SocketId, Error> reserve();
    bool install(SocketId id, std::shared_ptr<SocketBase> socket);
    [[nodiscard]] std::shared_ptr<SocketBase> acquire(SocketId id) const;
    void erase(SocketId id);
    [[nodiscard]] std::vector<std::shared_ptr<SocketBase>> snapshot() const;

private:
    const std::size_t max_sockets_;
    mutable std::mutex mutex_;
    std::unordered_map<SocketId, std::shared_ptr<SocketBase>> sockets_;
    std::atomic<std::uint32_t> next_id_{0};
};

struct Core final {
    asio::io_context& io_context;
    EventSink& sink;
    Config config;
    SocketDirectory directory;
    std::atomic<bool> accepting_events{true};

    Core(asio::io_context& context, EventSink& event_sink, Config value);
    bool publish(Event&& event) noexcept;
};

[[nodiscard]] std::string endpoint_text(const Tcp::endpoint& endpoint);
[[nodiscard]] std::string endpoint_text(const Udp::endpoint& endpoint);

class SocketBase : public std::enable_shared_from_this<SocketBase> {
public:
    SocketBase(Core& core, SocketId id, ServiceId owner);
    virtual ~SocketBase() = default;

    [[nodiscard]] SocketId id() const noexcept;
    [[nodiscard]] ServiceId owner_snapshot() const noexcept;
    [[nodiscard]] virtual SocketKind kind() const noexcept {
        return SocketKind::tcp_connection;
    }

    virtual void request_start(ServiceId new_owner) = 0;
    virtual void request_pause() = 0;
    virtual std::expected<void, Error> request_send(Buffer buffer, SendPriority priority) = 0;
    virtual void request_close(bool abort, ServiceId expected_owner = 0) = 0;
    virtual void request_nodelay(bool enabled) = 0;
    virtual std::expected<void, Error> request_udp_connect(Endpoint endpoint);
    virtual std::expected<void, Error> request_udp_send(Buffer buffer, Endpoint endpoint);
    std::future<SocketInfo> request_info();

protected:
    virtual SocketInfo socket_info() = 0; // Called exclusively on strand_.
    SocketInfo statistics(SocketType type) const;
    void record_read(std::size_t size);
    void record_write(std::size_t size);
    void change_owner(ServiceId owner) noexcept;
    bool publish(Event&& event) noexcept;
    void publish_close_once() noexcept;
    void publish_error(std::string message) noexcept;
    void unregister() noexcept;

    Core& core_;
    const SocketId id_;
    ServiceId owner_;
    std::atomic<ServiceId> owner_snapshot_;
    Strand strand_;
    bool close_sent_{false};
    std::uint64_t bytes_read_{}, bytes_written_{}, read_time_{}, write_time_{};
};

[[nodiscard]] std::expected<void, Error> validate_kind(
    const std::shared_ptr<SocketBase>& socket,
    SocketKind kind
);

[[nodiscard]] std::shared_ptr<SocketBase> make_outbound_connection(
    Core& core,
    SocketId id,
    ServiceId owner
);

[[nodiscard]] std::shared_ptr<SocketBase> make_accepted_connection(
    Core& core,
    SocketId id,
    ServiceId owner,
    Tcp::socket socket
);

void begin_connect(
    const std::shared_ptr<SocketBase>& socket,
    std::string host,
    std::uint16_t port
);

[[nodiscard]] std::shared_ptr<SocketBase> make_listener(
    Core& core,
    SocketId id,
    ServiceId owner
);

[[nodiscard]] std::future<std::expected<void, Error>> open_listener(
    const std::shared_ptr<SocketBase>& socket,
    std::string host,
    std::uint16_t port,
    int backlog
);

// Construct/open before publication in the directory. No handlers run yet.
[[nodiscard]] std::expected<std::shared_ptr<SocketBase>, Error> make_udp_socket(
    Core& core, SocketId id, ServiceId owner, std::string host, std::uint16_t port);
void begin_udp(const std::shared_ptr<SocketBase>& socket);
[[nodiscard]] std::expected<Endpoint, Error> resolve_udp(Endpoint endpoint);
[[nodiscard]] std::expected<std::shared_ptr<SocketBase>, Error> make_fd_stream(
    Core& core, SocketId id, ServiceId owner, int fd);
void begin_fd(const std::shared_ptr<SocketBase>& socket);

}  // namespace skynet::network::detail
