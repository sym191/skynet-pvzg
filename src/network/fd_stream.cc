#include "detail.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <deque>
#include <utility>

namespace skynet::network::detail {

class FdStream final : public SocketBase {
public:
    FdStream(Core& core, SocketId id, ServiceId owner)
        : SocketBase(core, id, owner), descriptor_(strand_) {}
    std::expected<void, Error> open(int fd) {
        const int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (duplicate < 0) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "cannot duplicate bound fd"});
        }
        asio::error_code error;
        descriptor_.assign(duplicate, error);
        if (error) {
            ::close(duplicate);
            return std::unexpected(Error{ErrorCode::invalid_argument, "cannot bind fd: " + error.message()});
        }
        descriptor_.non_blocking(true, error);
        if (error) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "cannot make fd nonblocking: " + error.message()});
        }
        const int flags = fcntl(duplicate, F_GETFL);
        readable_ = flags >= 0 && (flags & O_ACCMODE) != O_WRONLY;
        reading_ = readable_;
        return {};
    }
    SocketKind kind() const noexcept override { return SocketKind::fd; }
    void begin() {
        asio::post(strand_, [self = self()] {
            if (self->closed_) { return; }
            if (!self->publish(Event{.type = EventType::open, .destination = self->owner_,
                    .socket = self->id_, .text = "binding"})) { self->close(); return; }
            self->read();
        });
    }
    void request_start(ServiceId owner) override {
        asio::post(strand_, [self = self(), owner] {
            if (self->closing_) { return; }
            if (!self->core_.sink.owner_alive(owner)) { self->close(); return; }
            self->change_owner(owner);
            self->reading_ = self->readable_ && !self->eof_;
            if (!self->publish(Event{.type = EventType::open, .destination = owner,
                    .socket = self->id_, .text = "transfer"})) { self->close(); return; }
            self->read();
        });
    }
    void request_pause() override {
        asio::post(strand_, [self = self()] { self->reading_ = false; });
    }
    void request_nodelay(bool enabled) override {
        asio::post(strand_, [self = self(), enabled] {
            if (!self->closed_) {
                const int value = enabled;
                // Upstream ignores ENOTSOCK for stdin/pipes.
                setsockopt(self->descriptor_.native_handle(), IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
            }
        });
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
    std::expected<void, Error> request_send(Buffer buffer, SendPriority priority) override {
        if (!accepting_.load()) {
            return std::unexpected(Error{ErrorCode::invalid_socket, "bound fd is closing"});
        }
        if (buffer.empty()) { return {}; }
        const auto size = buffer.size();
        auto pending = pending_.load();
        do {
            if (size > core_.config.write_hard_limit - pending) {
                return std::unexpected(Error{ErrorCode::socket_limit_reached, "FD send queue hard limit exceeded"});
            }
        } while (!pending_.compare_exchange_weak(pending, pending + size));
        try {
            asio::post(strand_, [self = self(), buffer = std::move(buffer), priority]() mutable {
                if (self->closing_) { self->pending_.fetch_sub(buffer.size()); return; }
                auto& queue = priority == SendPriority::high ? self->high_ : self->low_;
                queue.push_back(std::make_shared<Buffer>(std::move(buffer)));
                self->warning();
                self->write();
            });
        } catch (...) {
            pending_.fetch_sub(size);
            return std::unexpected(Error{ErrorCode::invalid_argument, "failed to queue FD send"});
        }
        return {};
    }
private:
    std::shared_ptr<FdStream> self() { return std::static_pointer_cast<FdStream>(shared_from_this()); }
    SocketInfo socket_info() override {
        auto result = statistics(closed_ ? SocketType::unknown : SocketType::bound);
        result.reading = reading_;
        result.writing = writing_;
        result.queued_bytes = pending_.load();
        return result;
    }
    void read() {
        if (closed_ || !reading_ || receiving_ || eof_) { return; }
        if (!core_.sink.owner_alive(owner_)) { close(); return; }
        receiving_ = true;
        auto data = std::make_shared<Buffer>(core_.config.read_buffer_size);
        descriptor_.async_read_some(asio::buffer(*data),
            [self = self(), data](asio::error_code error, std::size_t size) {
                self->receiving_ = false;
                if (self->closing_) { return; }
                if (error) {
                    if (error == asio::error::eof) {
                        self->eof_ = true;
                        self->reading_ = false;
                        self->publish_close_once();
                    } else if (error != asio::error::operation_aborted) {
                        self->publish_error("FD read failed: " + error.message());
                        self->close();
                    }
                    return;
                }
                data->resize(size);
                self->record_read(size);
                if (!self->publish(Event{.type = EventType::data, .destination = self->owner_,
                        .socket = self->id_, .payload = std::move(*data)})) { self->close(); return; }
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
        auto data = queue.front();
        queue.pop_front();
        writing_ = true;
        asio::async_write(descriptor_, asio::buffer(*data),
            [self = self(), data](asio::error_code error, std::size_t size) {
                self->pending_.fetch_sub(data->size());
                self->writing_ = false;
                self->record_write(size);
                if (self->closed_) { return; }
                if (error) {
                    self->publish_error("FD write failed: " + error.message());
                    self->close();
                    return;
                }
                self->warning();
                self->write();
            });
    }
    void warning() {
        auto pending = pending_.load();
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
        closing_ = true;
        accepting_.store(false);
        reading_ = false;
        for (auto* queue : {&high_, &low_}) {
            for (const auto& data : *queue) { pending_.fetch_sub(data->size()); }
            queue->clear();
        }
        asio::error_code ignored;
        // Closing our duplicate never shuts down or closes the caller's FD.
        descriptor_.close(ignored);
        publish_close_once();
        unregister();
    }
    asio::posix::stream_descriptor descriptor_;
    std::deque<std::shared_ptr<Buffer>> high_, low_;
    std::atomic<std::size_t> pending_{};
    std::atomic<bool> accepting_{true};
    std::size_t next_warning_{core_.config.write_warning_bytes};
    bool readable_{true}, reading_{true}, receiving_{}, writing_{}, closing_{}, closed_{}, eof_{}, warned_{};
};
std::expected<std::shared_ptr<SocketBase>, Error> make_fd_stream(
    Core& core, SocketId id, ServiceId owner, int fd) {
    auto stream = std::make_shared<FdStream>(core, id, owner);
    if (auto result = stream->open(fd); !result) { return std::unexpected(result.error()); }
    return stream;
}
void begin_fd(const std::shared_ptr<SocketBase>& socket) {
    std::static_pointer_cast<FdStream>(socket)->begin();
}
}  // namespace skynet::network::detail
