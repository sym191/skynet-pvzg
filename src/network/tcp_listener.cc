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

    void open(std::string host, std::uint16_t port, int backlog) {
        auto self = std::static_pointer_cast<TcpListener>(shared_from_this());
        asio::post(
            strand_,
            [self, host = std::move(host), port, backlog]() mutable {
                self->do_open(std::move(host), port, backlog);
            }
        );
    }

    void request_start(ServiceId new_owner) override {
        auto self = std::static_pointer_cast<TcpListener>(shared_from_this());
        asio::post(strand_, [self, new_owner] {
            if (self->closed_) {
                return;
            }
            self->change_owner(new_owner);
            self->accept_enabled_ = true;
            self->begin_accept();
        });
    }

    void request_pause() override {
        auto self = std::static_pointer_cast<TcpListener>(shared_from_this());
        asio::post(strand_, [self] {
            self->accept_enabled_ = false;
        });
    }

    void request_send(Buffer, SendPriority) override {
    }

    void request_close(bool) override {
        auto self = std::static_pointer_cast<TcpListener>(shared_from_this());
        asio::post(strand_, [self] {
            self->finish_close(true);
        });
    }

    void request_nodelay(bool) override {
    }

private:
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
                    if (error || results.empty()) {
                        self->publish_error(
                            "listen resolve failed: " + error.message()
                        );
                        self->finish_close(false);
                        return;
                    }
                    self->open_endpoint(results.begin()->endpoint());
                }
            )
        );
    }

    void open_endpoint(const Tcp::endpoint& endpoint) {
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
            publish_error("listen failed: " + error.message());
            finish_close(false);
            return;
        }

        const auto local_endpoint = acceptor_.local_endpoint(error);
        publish(Event{
            .type = EventType::open,
            .destination = owner_,
            .socket = id_,
            .accepted_socket = 0,
            .value = 0,
            .port = static_cast<std::uint16_t>(
                error ? 0 : local_endpoint.port()
            ),
            .payload = {},
            .text = error ? std::string{} : endpoint_text(local_endpoint),
        });

        if (accept_enabled_) {
            begin_accept();
        }
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
                publish(Event{
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
                });
            }
        }

        if (accept_enabled_) {
            begin_accept();
        }
    }

    void finish_close(bool notify_close) {
        if (closed_) {
            return;
        }
        closed_ = true;
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

    Tcp::resolver resolver_;
    Tcp::acceptor acceptor_;
    int backlog_{128};
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

void open_listener(
    const std::shared_ptr<SocketBase>& socket,
    std::string host,
    std::uint16_t port,
    int backlog
) {
    auto listener = std::static_pointer_cast<TcpListener>(socket);
    listener->open(std::move(host), port, backlog);
}

}  // namespace skynet::network::detail
