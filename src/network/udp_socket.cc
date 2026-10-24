#include "detail.hpp"

#include <algorithm>
#include <deque>
#include <utility>

namespace skynet::network::detail {

std::expected<Endpoint, Error> resolve_udp(Endpoint endpoint) {
    if (endpoint.host.empty() || endpoint.port == 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "UDP target requires host and port"});
    }
    asio::io_context context;
    Udp::resolver resolver(context);
    asio::error_code error;
    auto results = resolver.resolve(Udp::v4(), endpoint.host, std::to_string(endpoint.port), error);
    if (error || results.empty()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "UDP resolve failed: " + error.message()});
    }
    return Endpoint{results.begin()->endpoint().address().to_string(), endpoint.port};
}

class UdpSocket final : public SocketBase {
public:
    UdpSocket(Core& core, SocketId id, ServiceId owner)
        : SocketBase(core, id, owner), socket_(strand_) {}

    std::expected<void, Error> open(std::string host, std::uint16_t port) {
        asio::error_code error;
        asio::ip::address_v4 address = asio::ip::address_v4::any();
        if (!host.empty() && host != "*") {
            address = asio::ip::make_address_v4(host, error);
            if (error) { return std::unexpected(Error{ErrorCode::invalid_argument, "invalid IPv4 bind address"}); }
        }
        socket_.open(Udp::v4(), error);
        // Match Skynet's SO_REUSEADDR for bound UDP sockets.
        if (!error && core_.config.reuse_address) {
            socket_.set_option(asio::socket_base::reuse_address(true), error);
        }
        if (!error) { socket_.bind(Udp::endpoint(address, port), error); }
        if (error) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "UDP bind failed: " + error.message()});
        }
        return {};
    }

    SocketKind kind() const noexcept override { return SocketKind::udp; }
    void begin() {
        asio::post(strand_, [self = self()] { self->read(); });
    }
    void request_start(ServiceId owner) override {
        asio::post(strand_, [self = self(), owner] {
            if (self->closing_) { return; }
            if (!self->core_.sink.owner_alive(owner)) { self->close(); return; }
            self->change_owner(owner);
            self->reading_ = true;
            if (!self->publish(Event{.type = EventType::open, .destination = owner,
                    .socket = self->id_, .text = "transfer"})) { self->close(); return; }
            self->read();
        });
    }
    void request_pause() override {
        asio::post(strand_, [self = self()] { self->reading_ = false; });
    }
    void request_close(bool abort, ServiceId expected_owner) override {
        asio::post(strand_, [self = self(), abort, expected_owner] {
            if (expected_owner && self->owner_ != expected_owner) { return; }
            self->accepting_.store(false);
            self->closing_ = true;
            self->reading_ = false;
            self->publish_close_once();
            if (abort || !self->writing_) { self->close(); }
        });
    }
    void request_nodelay(bool) override {}
    std::expected<void, Error> request_udp_connect(Endpoint endpoint) override {
        asio::error_code error;
        auto address = asio::ip::make_address_v4(endpoint.host, error);
        if (error || !endpoint.port) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "invalid IPv4 UDP target"});
        }
        asio::post(strand_, [self = self(), target = Udp::endpoint(address, endpoint.port)] {
            if (!self->closed_) { self->target_ = target; }
        });
        return {};
    }
    std::expected<void, Error> request_send(Buffer buffer, SendPriority priority) override {
        return queue_send(std::move(buffer), {}, priority);
    }
    std::expected<void, Error> request_udp_send(Buffer buffer, Endpoint endpoint) override {
        return queue_send(std::move(buffer), std::move(endpoint), SendPriority::high);
    }

private:
    std::expected<void, Error> queue_send(Buffer buffer, Endpoint endpoint, SendPriority priority) {
        if (!accepting_.load()) {
            return std::unexpected(Error{ErrorCode::invalid_socket, "UDP socket is closing"});
        }
        if (buffer.size() > 65507) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "IPv4 UDP payload exceeds 65507 bytes"});
        }
        Udp::endpoint target;
        if (!endpoint.host.empty()) {
            asio::error_code error;
            auto address = asio::ip::make_address_v4(endpoint.host, error);
            if (error || endpoint.port == 0) {
                return std::unexpected(Error{ErrorCode::invalid_argument, "invalid IPv4 UDP target"});
            }
            target = {address, endpoint.port};
        } else if (endpoint.port) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "UDP target host is missing"});
        }
        // Empty datagrams still occupy queue memory. Charge at least one byte.
        const auto size = buffer.size();
        const auto charge = std::max(size, std::size_t{1});
        auto pending = pending_.load();
        do {
            if (charge > core_.config.write_hard_limit - pending) {
                return std::unexpected(Error{ErrorCode::socket_limit_reached, "UDP send queue hard limit exceeded"});
            }
        } while (!pending_.compare_exchange_weak(pending, pending + charge));
        pending_bytes_.fetch_add(size);
        try {
            asio::post(strand_, [self = self(), buffer = std::move(buffer), target, charge, priority]() mutable {
                if (self->closing_) {
                    self->pending_.fetch_sub(charge);
                    self->pending_bytes_.fetch_sub(buffer.size());
                    return;
                }
                const auto destination = target.port() ? target : self->target_;
                if (!destination.port()) {
                    self->pending_.fetch_sub(charge);
                    self->pending_bytes_.fetch_sub(buffer.size());
                    self->publish_error("UDP default target is not set");
                    return;
                }
                auto& queue = priority == SendPriority::high ? self->high_ : self->low_;
                queue.push_back(std::make_shared<Packet>(Packet{std::move(buffer), destination, charge}));
                self->warning();
                self->write();
            });
        } catch (...) {
            pending_.fetch_sub(charge);
            pending_bytes_.fetch_sub(size);
            return std::unexpected(Error{ErrorCode::invalid_argument, "failed to queue UDP send"});
        }
        return {};
    }

private:
    struct Packet { Buffer data; Udp::endpoint target; std::size_t charge; };
    std::shared_ptr<UdpSocket> self() { return std::static_pointer_cast<UdpSocket>(shared_from_this()); }
    SocketInfo socket_info() override {
        auto info = statistics(closed_ ? SocketType::unknown : SocketType::udp);
        info.queued_bytes = pending_bytes_.load();
        info.reading = reading_;
        info.writing = writing_;
        if (target_.port()) { info.name = endpoint_text(target_); }
        return info;
    }
    void read() {
        if (!reading_ || receiving_ || closed_) { return; }
        if (!core_.sink.owner_alive(owner_)) { close(); return; }
        receiving_ = true;
        auto data = std::make_shared<Buffer>(core_.config.udp_buffer_size);
        auto source = std::make_shared<Udp::endpoint>();
        socket_.async_receive_from(asio::buffer(*data), *source,
            [self = self(), data, source](asio::error_code error, std::size_t size) {
                self->receiving_ = false;
                if (self->closed_ || self->closing_) { return; }
                if (error) {
                    if (error != asio::error::operation_aborted) {
                        self->publish_error("UDP receive failed: " + error.message());
                        self->close();
                    }
                    return;
                }
                data->resize(size); // Zero bytes are a datagram, never EOF.
                self->record_read(size);
                if (!self->publish(Event{.type = EventType::udp, .destination = self->owner_,
                        .socket = self->id_, .port = source->port(), .payload = std::move(*data),
                        .text = source->address().to_string()})) { self->close(); return; }
                self->read();
            });
    }
    void write() {
        if (writing_ || closed_) { return; }
        auto& queue = high_.empty() ? low_ : high_;
        if (queue.empty()) {
            warning();
            if (closing_) { close(); }
            return;
        }
        auto packet = queue.front();
        queue.pop_front();
        writing_ = true;
        // Completion owns the datagram even after close clears the queue.
        socket_.async_send_to(asio::buffer(packet->data), packet->target,
            [self = self(), packet](asio::error_code error, std::size_t size) {
                self->pending_.fetch_sub(packet->charge);
                self->pending_bytes_.fetch_sub(packet->data.size());
                self->writing_ = false;
                self->record_write(size);
                if (self->closed_) { return; }
                if (error) { self->publish_error("UDP send failed: " + error.message()); }
                self->warning();
                self->write();
            });
    }
    void warning() {
        auto pending = pending_bytes_.load();
        if (next_warning_ && pending >= next_warning_) {
            publish(Event{.type = EventType::warning, .destination = owner_, .socket = id_,
                .value = static_cast<std::uint32_t>((pending + 1023) / 1024)});
            warned_ = true;
            next_warning_ = std::max(next_warning_ * 2, pending + 1);
        } else if (!pending && warned_) {
            publish(Event{.type = EventType::warning, .destination = owner_, .socket = id_});
            warned_ = false;
            next_warning_ = core_.config.write_warning_bytes;
        }
    }
    void close() {
        if (std::exchange(closed_, true)) { return; }
        accepting_.store(false);
        closing_ = true;
        reading_ = false;
        for (auto* queue : {&high_, &low_}) {
            for (const auto& packet : *queue) {
                pending_.fetch_sub(packet->charge);
                pending_bytes_.fetch_sub(packet->data.size());
            }
            queue->clear();
        }
        asio::error_code ignored;
        socket_.close(ignored);
        publish_close_once();
        unregister();
    }
    Udp::socket socket_;
    Udp::endpoint target_;
    std::deque<std::shared_ptr<Packet>> high_, low_;
    std::atomic<std::size_t> pending_{}, pending_bytes_{};
    std::atomic<bool> accepting_{true};
    std::size_t next_warning_{core_.config.write_warning_bytes};
    bool reading_{true}, receiving_{}, writing_{}, closing_{}, closed_{}, warned_{};
};

std::expected<std::shared_ptr<SocketBase>, Error> make_udp_socket(
    Core& core, SocketId id, ServiceId owner, std::string host, std::uint16_t port) {
    auto socket = std::make_shared<UdpSocket>(core, id, owner);
    if (auto result = socket->open(std::move(host), port); !result) {
        return std::unexpected(result.error());
    }
    return socket;
}
void begin_udp(const std::shared_ptr<SocketBase>& socket) {
    std::static_pointer_cast<UdpSocket>(socket)->begin();
}
}  // namespace skynet::network::detail
