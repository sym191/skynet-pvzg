#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace skynet::network {

using ServiceId = std::uint32_t;
using SocketId = std::int32_t;
using Buffer = std::vector<std::byte>;

enum class EventType : std::uint8_t {
    data,
    close,
    open,
    accept,
    error,
    warning,
};

struct Event {
    EventType type{};
    ServiceId destination{};
    SocketId socket{};
    SocketId accepted_socket{};
    std::uint32_t value{};
    std::uint16_t port{};
    Buffer payload;
    std::string text;
};

class EventSink {
public:
    virtual ~EventSink() = default;
    virtual bool publish(Event&& event) noexcept = 0;
};

enum class ErrorCode : std::uint8_t {
    invalid_argument,
    invalid_socket,
    wrong_owner,
    socket_limit_reached,
    runtime_not_running,
    runtime_stopping,
};

struct Error {
    ErrorCode code{};
    std::string message;
};

enum class SendPriority : std::uint8_t {
    high,
    low,
};

struct Config {
    std::size_t thread_count{1};
    std::size_t max_sockets{65'536};
    std::size_t read_buffer_size{16 * 1024};
    std::size_t write_warning_bytes{1024 * 1024};
    std::size_t write_hard_limit{64 * 1024 * 1024};
    std::uint32_t connect_timeout_ms{10'000};
    bool reuse_address{true};
    bool keep_alive{true};
};

class Runtime final {
public:
    explicit Runtime(EventSink& sink, Config config = {});
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    [[nodiscard]] std::expected<void, Error> start();
    void request_stop() noexcept;
    void join() noexcept;

    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] std::expected<SocketId, Error> listen(
        ServiceId owner,
        std::string host,
        std::uint16_t port,
        int backlog = 128
    );

    [[nodiscard]] std::expected<SocketId, Error> connect(
        ServiceId owner,
        std::string host,
        std::uint16_t port
    );

    [[nodiscard]] std::expected<void, Error> start_socket(
        ServiceId new_owner,
        SocketId socket
    );

    [[nodiscard]] std::expected<void, Error> pause_socket(
        ServiceId owner,
        SocketId socket
    );

    [[nodiscard]] std::expected<void, Error> send(
        ServiceId owner,
        SocketId socket,
        Buffer buffer,
        SendPriority priority = SendPriority::high
    );

    [[nodiscard]] std::expected<void, Error> send(
        ServiceId owner,
        SocketId socket,
        std::span<const std::byte> data,
        SendPriority priority = SendPriority::high
    );

    [[nodiscard]] std::expected<void, Error> close_socket(
        ServiceId owner,
        SocketId socket
    );

    [[nodiscard]] std::expected<void, Error> shutdown_socket(
        ServiceId owner,
        SocketId socket
    );

    [[nodiscard]] std::expected<void, Error> set_nodelay(
        ServiceId owner,
        SocketId socket,
        bool enabled = true
    );

    void close_all(ServiceId owner) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] Buffer copy_buffer(std::string_view text);

}  // namespace skynet::network
