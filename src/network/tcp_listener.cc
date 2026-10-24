#include "detail.hpp"

#include <utility>

namespace skynet::network::detail {

class TcpListener final : public SocketBase {
public:
    TcpListener(Core& core, SocketId id, ServiceId owner)
        : SocketBase(core, id, owner),
          resolver_(strand_),
          acceptor_(strand_) {
    }

    std::future<std::expected<void, Error>> open(std::string host, std::uint16_t port, int backlog) {
        auto result = open_result_.get_future();
        auto self = std::static_pointer_cast<TcpListener>(shared_from_this());
        asio::post(
            strand_,
            [self, host = std::move(host), port, backlog]() mutable {
                self->do_open(std::move(host), port, backlog);
            }
        );
        return result;
    }

    void request_start(ServiceId new_owner) override {
        auto self = std::static_pointer_cast<TcpListener>(shared_from_this());
        asio::post(strand_, [self, new_owner] {
            if (self->closed_) {
                return;
            }
            if (!self->core_.sink.owner_alive(new_owner)) {
                self->finish_close(true);
                return;
            }
            self->change_owner(new_owner);
            self->accept_enabled_ = true;
            self->started_ = true;
            if (self->acceptor_.is_open() && !self->acknowledge_start()) { return; }
            self->begin_accept();
        });
    }

    void request_pause() override {
        auto self = std::static_pointer_cast<TcpListener>(shared_from_this());
        asio::post(strand_, [self] {
            self->accept_enabled_ = false;
        });
    }

    std::expected<void, Error> request_send(Buffer, SendPriority) override {
        return std::unexpected(Error{ErrorCode::wrong_socket_kind, "cannot send on a listener"});
    }

    void request_close(bool, ServiceId expected_owner = 0) override {
        auto self = std::static_pointer_cast<TcpListener>(shared_from_this());
        asio::post(strand_, [self, expected_owner] {
            if (expected_owner != 0 && self->owner_ != expected_owner) { return; }
            self->finish_close(true);
        });
    }

    void request_nodelay(bool) override {
    }

    SocketKind kind() const noexcept override { return SocketKind::tcp_listener; }

private:
    SocketInfo socket_info() override {
        auto result = statistics(closed_ || !started_ ? SocketType::unknown : SocketType::listen);
        asio::error_code error;
        auto local = acceptor_.local_endpoint(error);
        if (!error) { result.name = endpoint_text(local); }
        result.reading = accept_enabled_;
        return result;
    }
    void do_open(std::string host, std::uint16_t port, int backlog) {
        if (closed_) {
            return;
        }
        backlog_ = backlog;

        if (host.empty() || host == "*" || host == "0.0.0.0") {
            open_endpoint(Tcp::endpoint(Tcp::v4(), port));
            return;
        }
        if (host == "::") {
            open_endpoint(Tcp::endpoint(Tcp::v6(), port));
            return;
        }

        asio::error_code address_error;
        const auto address = asio::ip::make_address(host, address_error);
        if (!address_error) {
            open_endpoint(Tcp::endpoint(address, port));
            return;
        }

        auto self = std::static_pointer_cast<TcpListener>(shared_from_this());
        resolver_.async_resolve(
            std::move(host),
            std::to_string(port),
            asio::bind_executor(
                strand_,
                [self](const asio::error_code& error,
                       Tcp::resolver::results_type results) {
                    if (self->closed_) { return; }
                    if (error || results.empty()) {
                        self->complete_open(std::unexpected(Error{ErrorCode::invalid_argument,
                            "listen resolve failed: " + error.message()}));
                        self->finish_close(false);
                        return;
                    }
                    self->open_endpoint(results.begin()->endpoint());
                }
            )
        );
    }

    void open_endpoint(const Tcp::endpoint& endpoint) {
        if (closed_) { return; }
        asio::error_code error;
        acceptor_.open(endpoint.protocol(), error);
        if (!error && core_.config.reuse_address) {
            acceptor_.set_option(asio::socket_base::reuse_address(true), error);
        }
        if (!error) {
            acceptor_.bind(endpoint, error);
        }
        if (!error) {
            acceptor_.listen(backlog_, error);
        }
        if (error) {
            complete_open(std::unexpected(Error{ErrorCode::invalid_argument,
                "listen failed: " + error.message()}));
            finish_close(false);
            return;
        }

        const auto local_endpoint = acceptor_.local_endpoint(error);
        if (!publish(Event{
            .type = EventType::open,
            .destination = owner_,
            .socket = id_,
            .accepted_socket = 0,
            .value = error ? 0U : local_endpoint.port(),
            .port = static_cast<std::uint16_t>(
                error ? 0 : local_endpoint.port()
            ),
            .payload = {},
            .text = error ? std::string{} : endpoint_text(local_endpoint),
        })) { finish_close(false); return; }

        complete_open({});
        if (accept_enabled_ && acknowledge_start()) { begin_accept(); }
    }

    bool acknowledge_start() {
        if (!publish(Event{.type = EventType::open, .destination = owner_,
                .socket = id_, .payload = {}, .text = "start"})) {
            finish_close(false);
            return false;
        }
        return true;
    }

    void begin_accept() {
        if (closed_ || !accept_enabled_ || accept_in_flight_ ||
            !acceptor_.is_open()) {
            return;
        }
        accept_in_flight_ = true;
        auto self = std::static_pointer_cast<TcpListener>(shared_from_this());
        acceptor_.async_accept(asio::bind_executor(
            strand_,
            [self](const asio::error_code& error, Tcp::socket socket) mutable {
                self->handle_accept(error, std::move(socket));
            }
        ));
    }

    void handle_accept(const asio::error_code& error, Tcp::socket socket) {
        accept_in_flight_ = false;
        if (closed_) {
            return;
        }
        if (error) {
            if (error != asio::error::operation_aborted) {
                publish_error("accept failed: " + error.message());
            }
            if (accept_enabled_) {
                begin_accept();
            }
            return;
        }

        record_read(1);
        auto reserved = core_.directory.reserve();
        if (!reserved) {
            asio::error_code ignored;
            socket.close(ignored);
            publish_error(reserved.error().message);
        } else {
            const SocketId accepted_id = *reserved;
            asio::error_code endpoint_error;
            const auto remote = socket.remote_endpoint(endpoint_error);
            auto connection = make_accepted_connection(
                core_,
                accepted_id,
                owner_,
                std::move(socket)
            );
            if (!core_.directory.install(accepted_id, connection)) {
                core_.directory.erase(accepted_id);
                connection->request_close(true);
                publish_error("failed to install accepted socket");
            } else {
                if (!publish(Event{
                    .type = EventType::accept,
                    .destination = owner_,
                    .socket = id_,
                    .accepted_socket = accepted_id,
                    .value = 0,
                    .port = static_cast<std::uint16_t>(
                        endpoint_error ? 0 : remote.port()
                    ),
                    .payload = {},
                    .text = endpoint_error
                        ? std::string{}
                        : endpoint_text(remote),
                })) { connection->request_close(true); }
            }
        }

        if (accept_enabled_) {
            begin_accept();
        }
    }

    void complete_open(std::expected<void, Error> result) {
        if (!std::exchange(open_completed_, true)) { open_result_.set_value(std::move(result)); }
    }

    void finish_close(bool notify_close) {
        if (closed_) {
            return;
        }
        closed_ = true;
        complete_open(std::unexpected(Error{ErrorCode::runtime_stopping, "listener closed before ready"}));
        accept_enabled_ = false;
        resolver_.cancel();
        asio::error_code ignored;
        acceptor_.cancel(ignored);
        acceptor_.close(ignored);
        if (notify_close) {
            publish_close_once();
        }
        unregister();
    }

    std::promise<std::expected<void, Error>> open_result_;
    bool open_completed_{false};
    Tcp::resolver resolver_;
    Tcp::acceptor acceptor_;
    int backlog_{128};
    bool started_{false};
    bool accept_enabled_{false};
    bool accept_in_flight_{false};
    bool closed_{false};
};

std::shared_ptr<SocketBase> make_listener(
    Core& core,
    SocketId id,
    ServiceId owner
) {
    return std::make_shared<TcpListener>(core, id, owner);
}

std::future<std::expected<void, Error>> open_listener(
    const std::shared_ptr<SocketBase>& socket,
    std::string host,
    std::uint16_t port,
    int backlog
) {
    auto listener = std::static_pointer_cast<TcpListener>(socket);
    return listener->open(std::move(host), port, backlog);
}

}  // namespace skynet::network::detail
