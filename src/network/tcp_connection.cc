#include "detail.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <optional>
#include <utility>

namespace skynet::network::detail {

class TcpConnection final : public SocketBase {
public:
    TcpConnection(Core& core, SocketId id, ServiceId owner)
        : SocketBase(core, id, owner),
          resolver_(strand_),
          connect_timer_(strand_),
          socket_(strand_) {
    }

    TcpConnection(
        Core& core,
        SocketId id,
        ServiceId owner,
        Tcp::socket socket
    )
        : SocketBase(core, id, owner),
          resolver_(strand_),
          connect_timer_(strand_),
          socket_(std::move(socket)),
          state_(State::paused),
          accepting_writes_(true) {
        apply_common_options();
    }

    void begin_connect(std::string host, std::uint16_t port) {
        auto self = std::static_pointer_cast<TcpConnection>(shared_from_this());
        asio::post(
            strand_,
            [self, host = std::move(host), port]() mutable {
                self->do_resolve(std::move(host), port);
            }
        );
    }

    void request_start(ServiceId new_owner) override {
        auto self = std::static_pointer_cast<TcpConnection>(shared_from_this());
        asio::post(strand_, [self, new_owner] {
            if (self->state_ == State::closed ||
                self->state_ == State::closing) {
                return;
            }
            if (!self->core_.sink.owner_alive(new_owner)) {
                self->abort_close();
                return;
            }
            self->change_owner(new_owner);
            self->read_enabled_ = true;
            if (self->state_ == State::paused) {
                self->state_ = State::connected;
            }
            if (self->state_ == State::connected) {
                if (!self->publish(Event{.type = EventType::open, .destination = new_owner,
                        .socket = self->id_, .payload = {}, .text = self->started_ ? "transfer" : "start"})) {
                    self->abort_close();
                    return;
                }
                self->started_ = true;
            }
            self->begin_read();
        });
    }

    void request_pause() override {
        auto self = std::static_pointer_cast<TcpConnection>(shared_from_this());
        asio::post(strand_, [self] {
            self->read_enabled_ = false;
            if (self->state_ == State::connected) {
                self->state_ = State::paused;
            }
        });
    }

    std::expected<void, Error> request_send(Buffer buffer, SendPriority priority) override {
        if (!accepting_writes_.load(std::memory_order_acquire)) {
            return std::unexpected(Error{ErrorCode::invalid_socket, "socket is closing"});
        }
        const auto size = buffer.size();
        auto pending = pending_write_bytes_.load(std::memory_order_relaxed);
        while (true) {
            if (size > core_.config.write_hard_limit - pending) {
                if (accepting_writes_.exchange(false, std::memory_order_acq_rel)) {
                    auto self = std::static_pointer_cast<TcpConnection>(shared_from_this());
                    asio::post(strand_, [self] {
                        self->publish_error("send queue hard limit exceeded");
                        self->abort_close();
                    });
                }
                return std::unexpected(Error{ErrorCode::socket_limit_reached, "send queue hard limit exceeded"});
            }
            if (pending_write_bytes_.compare_exchange_weak(pending, pending + size)) { break; }
        }
        // Reserve before post: executor closures count against the same budget
        // as queued and in-flight writes. An abort keeps submitted bytes alive.
        try {
            auto self = std::static_pointer_cast<TcpConnection>(shared_from_this());
            asio::post(strand_, [self, buffer = std::move(buffer), priority]() mutable {
                self->enqueue_send(std::move(buffer), priority);
            });
        } catch (...) {
            pending_write_bytes_.fetch_sub(size);
            return std::unexpected(Error{ErrorCode::invalid_argument, "failed to queue send"});
        }
        return {};
    }

    void request_close(bool abort, ServiceId expected_owner = 0) override {
        auto self = std::static_pointer_cast<TcpConnection>(shared_from_this());
        asio::post(strand_, [self, abort, expected_owner] {
            if (expected_owner != 0 && self->owner_ != expected_owner) { return; }
            if (abort) {
                self->abort_close();
            } else {
                self->graceful_close();
            }
        });
    }

    void request_nodelay(bool enabled) override {
        auto self = std::static_pointer_cast<TcpConnection>(shared_from_this());
        asio::post(strand_, [self, enabled] {
            if (!self->socket_.is_open()) {
                return;
            }
            asio::error_code error;
            self->socket_.set_option(Tcp::no_delay(enabled), error);
            if (error) {
                self->publish_error(
                    "set TCP_NODELAY failed: " + error.message()
                );
            }
        });
    }

private:
    enum class State : std::uint8_t {
        resolving,
        connecting,
        paused,
        connected,
        closing,
        closed,
    };

    void do_resolve(std::string host, std::uint16_t port) {
        if (state_ == State::closing || state_ == State::closed) {
            return;
        }
        state_ = State::resolving;

        connect_timer_.expires_after(
            std::chrono::milliseconds(core_.config.connect_timeout_ms)
        );
        auto self = std::static_pointer_cast<TcpConnection>(shared_from_this());
        connect_timer_.async_wait(asio::bind_executor(
            strand_,
            [self](const asio::error_code& error) {
                if (!error) {
                    self->publish_error("connect timeout");
                    self->abort_close();
                }
            }
        ));

        resolver_.async_resolve(
            std::move(host),
            std::to_string(port),
            asio::bind_executor(
                strand_,
                [self](const asio::error_code& error,
                       Tcp::resolver::results_type results) {
                    if (self->state_ == State::closing || self->state_ == State::closed) { return; }
                    if (error) {
                        self->connect_timer_.cancel();
                        self->publish_error(
                            "resolve failed: " + error.message()
                        );
                        self->finish_close(false);
                        return;
                    }
                    self->state_ = State::connecting;
                    asio::async_connect(
                        self->socket_,
                        results,
                        asio::bind_executor(
                            self->strand_,
                            [self](const asio::error_code& connect_error,
                                   const Tcp::endpoint& endpoint) {
                                self->handle_connect(connect_error, endpoint);
                            }
                        )
                    );
                }
            )
        );
    }

    void handle_connect(
        const asio::error_code& error,
        const Tcp::endpoint& endpoint
    ) {
        connect_timer_.cancel();
        if (state_ == State::closing || state_ == State::closed) {
            return;
        }
        if (error) {
            publish_error("connect failed: " + error.message());
            finish_close(false);
            return;
        }

        apply_common_options();
        state_ = State::connected;
        read_enabled_ = true;
        started_ = true;
        accepting_writes_.store(true, std::memory_order_release);
        if (!publish(Event{
            .type = EventType::open,
            .destination = owner_,
            .socket = id_,
            .accepted_socket = 0,
            .value = 0,
            .port = endpoint.port(),
            .payload = {},
            .text = endpoint_text(endpoint),
        })) { abort_close(); return; }
        begin_read();
    }

    void apply_common_options() {
        if (!socket_.is_open()) {
            return;
        }
        asio::error_code error;
        socket_.set_option(
            asio::socket_base::keep_alive(core_.config.keep_alive),
            error
        );
    }

    void begin_read() {
        if (!read_enabled_ || read_in_flight_ ||
            state_ != State::connected || read_eof_ || !socket_.is_open()) {
            return;
        }

        read_in_flight_ = true;
        auto buffer = std::make_shared<Buffer>(core_.config.read_buffer_size);
        auto self = std::static_pointer_cast<TcpConnection>(shared_from_this());
        socket_.async_read_some(
            asio::buffer(buffer->data(), buffer->size()),
            asio::bind_executor(
                strand_,
                [self, buffer](const asio::error_code& error,
                               std::size_t size) {
                    self->handle_read(error, size, buffer);
                }
            )
        );
    }

    void handle_read(
        const asio::error_code& error,
        std::size_t size,
        const std::shared_ptr<Buffer>& buffer
    ) {
        read_in_flight_ = false;
        if (state_ == State::closing || state_ == State::closed) {
            return;
        }

        if (error) {
            if (error == asio::error::operation_aborted) {
                return;
            }
            if (error == asio::error::eof) {
                // Skynet keeps the write half usable after remote EOF.
                read_eof_ = true;
                read_enabled_ = false;
                publish_close_once();
            } else {
                publish_error("read failed: " + error.message());
                abort_close();
            }
            return;
        }

        buffer->resize(size);
        if (!publish(Event{
            .type = EventType::data,
            .destination = owner_,
            .socket = id_,
            .accepted_socket = 0,
            .value = static_cast<std::uint32_t>(size),
            .port = 0,
            .payload = std::move(*buffer),
            .text = {},
        })) { abort_close(); return; }

        if (read_enabled_) {
            begin_read();
        } else {
            state_ = State::paused;
        }
    }

    void enqueue_send(Buffer buffer, SendPriority priority) {
        if (state_ == State::closing || state_ == State::closed ||
            !socket_.is_open() || buffer.empty()) {
            pending_write_bytes_.fetch_sub(buffer.size());
            return;
        }

        queued_write_bytes_ += buffer.size();
        auto& queue = priority == SendPriority::high
            ? high_queue_
            : low_queue_;
        queue.push_back(std::move(buffer));
        update_warning();
        begin_write();
    }

    void begin_write() {
        if (write_in_flight_ || state_ == State::closed ||
            !socket_.is_open()) {
            return;
        }

        if (!high_queue_.empty()) {
            active_write_ = std::move(high_queue_.front());
            high_queue_.pop_front();
        } else if (!low_queue_.empty()) {
            active_write_ = std::move(low_queue_.front());
            low_queue_.pop_front();
        } else {
            if (state_ == State::closing) {
                finish_close(true);
            }
            return;
        }

        write_in_flight_ = true;
        auto self = std::static_pointer_cast<TcpConnection>(shared_from_this());
        asio::async_write(
            socket_,
            asio::buffer(active_write_->data(), active_write_->size()),
            asio::bind_executor(
                strand_,
                [self](const asio::error_code& error, std::size_t size) {
                    self->handle_write(error, size);
                }
            )
        );
    }

    void handle_write(const asio::error_code& error, std::size_t) {
        write_in_flight_ = false;
        if (active_write_) {
            queued_write_bytes_ -= active_write_->size();
            pending_write_bytes_.fetch_sub(active_write_->size());
            active_write_.reset();
        }

        if (state_ == State::closed) {
            return;
        }
        if (error) {
            if (error != asio::error::operation_aborted) {
                publish_error("write failed: " + error.message());
            }
            abort_close();
            return;
        }

        update_warning();
        begin_write();
    }

    void update_warning() {
        const auto threshold = core_.config.write_warning_bytes;
        if (threshold == 0) {
            return;
        }

        if (queued_write_bytes_ >= next_warning_bytes_) {
            publish(Event{
                .type = EventType::warning,
                .destination = owner_,
                .socket = id_,
                .accepted_socket = 0,
                .value = static_cast<std::uint32_t>(
                    queued_write_bytes_ / 1024
                ),
                .port = 0,
                .payload = {},
                .text = {},
            });
            warning_active_ = true;
            next_warning_bytes_ = std::max(
                next_warning_bytes_ * 2,
                threshold
            );
        } else if (queued_write_bytes_ == 0) {
            next_warning_bytes_ = threshold;
            if (std::exchange(warning_active_, false)) {
                publish(Event{.type = EventType::warning, .destination = owner_,
                    .socket = id_, .value = 0, .payload = {}, .text = {}});
            }
        }
    }

    void graceful_close() {
        if (state_ == State::closed || state_ == State::closing) {
            return;
        }
        state_ = State::closing;
        accepting_writes_.store(false, std::memory_order_release);
        read_enabled_ = false;
        resolver_.cancel();
        connect_timer_.cancel();
        // Wake socket.close immediately; queued writes still drain afterward.
        publish_close_once();
        asio::error_code ignored;
        socket_.shutdown(Tcp::socket::shutdown_receive, ignored);
        if (!write_in_flight_ && high_queue_.empty() && low_queue_.empty()) {
            finish_close(true);
        }
    }

    void abort_close() {
        if (state_ == State::closed) {
            return;
        }
        state_ = State::closing;
        accepting_writes_.store(false, std::memory_order_release);
        read_enabled_ = false;
        resolver_.cancel();
        connect_timer_.cancel();
        for (const auto& buffer : high_queue_) { pending_write_bytes_.fetch_sub(buffer.size()); }
        for (const auto& buffer : low_queue_) { pending_write_bytes_.fetch_sub(buffer.size()); }
        high_queue_.clear();
        low_queue_.clear();
        // Cancellation is asynchronous: keep the submitted bytes until completion.
        if (!write_in_flight_) { active_write_.reset(); }
        queued_write_bytes_ = active_write_ ? active_write_->size() : 0;
        finish_close(true);
    }

    void finish_close(bool notify_close) {
        if (state_ == State::closed) {
            return;
        }
        state_ = State::closed;
        accepting_writes_.store(false, std::memory_order_release);
        asio::error_code ignored;
        socket_.cancel(ignored);
        socket_.shutdown(Tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
        if (notify_close) {
            publish_close_once();
        }
        unregister();
    }

    Tcp::resolver resolver_;
    asio::steady_timer connect_timer_;
    Tcp::socket socket_;
    State state_{State::resolving};
    std::atomic<bool> accepting_writes_{false};
    std::atomic<std::size_t> pending_write_bytes_{0};
    bool warning_active_{false};
    bool read_eof_{false};
    bool started_{false};
    bool read_enabled_{false};
    bool read_in_flight_{false};
    bool write_in_flight_{false};
    std::deque<Buffer> high_queue_;
    std::deque<Buffer> low_queue_;
    std::optional<Buffer> active_write_;
    std::size_t queued_write_bytes_{0};
    std::size_t next_warning_bytes_{core_.config.write_warning_bytes};
};

std::shared_ptr<SocketBase> make_outbound_connection(
    Core& core,
    SocketId id,
    ServiceId owner
) {
    return std::make_shared<TcpConnection>(core, id, owner);
}

std::shared_ptr<SocketBase> make_accepted_connection(
    Core& core,
    SocketId id,
    ServiceId owner,
    Tcp::socket socket
) {
    return std::make_shared<TcpConnection>(
        core,
        id,
        owner,
        std::move(socket)
    );
}

void begin_connect(
    const std::shared_ptr<SocketBase>& socket,
    std::string host,
    std::uint16_t port
) {
    auto connection = std::static_pointer_cast<TcpConnection>(socket);
    connection->begin_connect(std::move(host), port);
}

}  // namespace skynet::network::detail
