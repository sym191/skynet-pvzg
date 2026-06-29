#include "detail.hpp"

#include <limits>
#include <utility>

namespace skynet::network::detail {

SocketDirectory::SocketDirectory(std::size_t max_sockets)
    : max_sockets_(max_sockets) {
}

std::expected<SocketId, Error> SocketDirectory::reserve() {
    std::scoped_lock lock(mutex_);
    if (sockets_.size() >= max_sockets_) {
        return std::unexpected(Error{
            ErrorCode::socket_limit_reached,
            "network socket limit reached",
        });
    }

    for (std::size_t attempt = 0; attempt < max_sockets_ + 1; ++attempt) {
        auto value = next_id_.fetch_add(1, std::memory_order_relaxed) + 1;
        value &= static_cast<std::uint32_t>(
            std::numeric_limits<SocketId>::max()
        );
        if (value == 0) {
            continue;
        }

        const auto id = static_cast<SocketId>(value);
        if (!sockets_.contains(id)) {
            sockets_.emplace(id, nullptr);
            return id;
        }
    }

    return std::unexpected(Error{
        ErrorCode::socket_limit_reached,
        "unable to allocate a free socket id",
    });
}

bool SocketDirectory::install(
    SocketId id,
    std::shared_ptr<SocketBase> socket
) {
    std::scoped_lock lock(mutex_);
    auto iterator = sockets_.find(id);
    if (iterator == sockets_.end() || iterator->second) {
        return false;
    }
    iterator->second = std::move(socket);
    return true;
}

std::shared_ptr<SocketBase> SocketDirectory::acquire(SocketId id) const {
    std::scoped_lock lock(mutex_);
    const auto iterator = sockets_.find(id);
    if (iterator == sockets_.end()) {
        return {};
    }
    return iterator->second;
}

void SocketDirectory::erase(SocketId id) {
    std::scoped_lock lock(mutex_);
    sockets_.erase(id);
}

std::vector<std::shared_ptr<SocketBase>> SocketDirectory::snapshot() const {
    std::vector<std::shared_ptr<SocketBase>> result;
    std::scoped_lock lock(mutex_);
    result.reserve(sockets_.size());
    for (const auto& [id, socket] : sockets_) {
        static_cast<void>(id);
        if (socket) {
            result.push_back(socket);
        }
    }
    return result;
}

Core::Core(asio::io_context& context, EventSink& event_sink, Config value)
    : io_context(context),
      sink(event_sink),
      config(std::move(value)),
      directory(config.max_sockets) {
}

bool Core::publish(Event&& event) noexcept {
    if (!accepting_events.load(std::memory_order_acquire)) {
        return false;
    }
    return sink.publish(std::move(event));
}

namespace {

template <class EndpointType>
std::string format_endpoint(const EndpointType& endpoint) {
    const auto endpoint_address = endpoint.address();
    const auto address = endpoint_address.to_string();
    if (endpoint_address.is_v6()) {
        return "[" + address + "]:" + std::to_string(endpoint.port());
    }
    return address + ":" + std::to_string(endpoint.port());
}

}  // namespace

std::string endpoint_text(const Tcp::endpoint& endpoint) {
    return format_endpoint(endpoint);
}

std::string endpoint_text(const Udp::endpoint& endpoint) {
    return format_endpoint(endpoint);
}

SocketBase::SocketBase(Core& core, SocketId id, ServiceId owner)
    : core_(core),
      id_(id),
      owner_(owner),
      owner_snapshot_(owner),
      strand_(asio::make_strand(core.io_context)) {
}

SocketId SocketBase::id() const noexcept {
    return id_;
}

ServiceId SocketBase::owner_snapshot() const noexcept {
    return owner_snapshot_.load(std::memory_order_acquire);
}

void SocketBase::request_udp_connect(Endpoint) {
    publish_error("operation requires a UDP socket");
}

void SocketBase::request_udp_send(Buffer, Endpoint) {
    publish_error("operation requires a UDP socket");
}

void SocketBase::change_owner(ServiceId owner) noexcept {
    owner_ = owner;
    owner_snapshot_.store(owner, std::memory_order_release);
}

bool SocketBase::publish(Event&& event) noexcept {
    return core_.publish(std::move(event));
}

void SocketBase::publish_close_once() noexcept {
    if (close_sent_) {
        return;
    }
    close_sent_ = true;
    publish(Event{
        .type = EventType::close,
        .destination = owner_,
        .socket = id_,
        .accepted_socket = 0,
        .value = 0,
        .port = 0,
        .payload = {},
        .text = {},
    });
}

void SocketBase::publish_error(std::string message) noexcept {
    publish(Event{
        .type = EventType::error,
        .destination = owner_,
        .socket = id_,
        .accepted_socket = 0,
        .value = 0,
        .port = 0,
        .payload = {},
        .text = std::move(message),
    });
}

void SocketBase::unregister() noexcept {
    core_.directory.erase(id_);
}

std::expected<void, Error> validate_owner(
    const std::shared_ptr<SocketBase>& socket,
    ServiceId owner
) {
    if (!socket) {
        return std::unexpected(Error{
            ErrorCode::invalid_socket,
            "socket id is not active",
        });
    }
    if (socket->owner_snapshot() != owner) {
        return std::unexpected(Error{
            ErrorCode::wrong_owner,
            "socket is owned by another service",
        });
    }
    return {};
}

std::expected<void, Error> validate_kind(
    const std::shared_ptr<SocketBase>& socket,
    SocketKind kind
) {
    if (!socket) {
        return std::unexpected(Error{
            ErrorCode::invalid_socket,
            "socket id is not active",
        });
    }
    if (socket->kind() != kind) {
        return std::unexpected(Error{
            ErrorCode::wrong_socket_kind,
            "socket has the wrong kind for this operation",
        });
    }
    return {};
}

}  // namespace skynet::network::detail
