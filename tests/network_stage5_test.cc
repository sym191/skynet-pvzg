#include "skynet/network/runtime.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <asio.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
namespace net = skynet::network;
using namespace std::chrono_literals;
void check(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
class Sink final : public net::EventSink {
public:
    bool publish(net::Event&& event) noexcept override {
        const bool reject = reject_udp && event.type == net::EventType::udp;
        std::unique_lock lock(mutex);
        bool block = event.type == net::EventType::open && block_open.exchange(false);
        events.push_back(std::move(event));
        ready.notify_all();
        if (block) { ready.wait(lock, [&] { return released; }); }
        return !reject;
    }
    bool owner_alive(net::ServiceId owner) const noexcept override { return owner != retired; }
    net::Event wait(net::EventType type, int id) {
        std::unique_lock lock(mutex);
        auto find = [&] { return std::ranges::find_if(events, [&](auto& event) {
            return event.type == type && event.socket == id;
        }); };
        check(ready.wait_for(lock, 4s, [&] { return find() != events.end(); }), "event timeout");
        auto result = std::move(*find());
        events.erase(find());
        return result;
    }
    void release() { std::lock_guard lock(mutex); released = true; ready.notify_all(); }
    std::atomic<bool> reject_udp{}, block_open{};
    std::atomic<net::ServiceId> retired{};
private:
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<net::Event> events;
    bool released{};
};
struct Release { Sink& sink; ~Release() { sink.release(); } };
struct Fd { int value{-1}; ~Fd() { if (value >= 0) { ::close(value); } } };
net::SocketInfo info(net::Runtime& runtime, int id) {
    for (auto& entry : runtime.info()) { if (entry.id == id) { return entry; } }
    throw std::runtime_error("missing socket info");
}
void gone(net::Runtime& runtime, int id) {
    // The info operation queued after CLOSE also serves as a strand barrier.
    for (const auto& entry : runtime.info()) { check(entry.id != id, "closed socket remained registered"); }
}
std::pair<net::Buffer, asio::ip::udp::endpoint> receive(asio::ip::udp::socket& socket) {
    socket.non_blocking(true);
    net::Buffer data(65536);
    asio::ip::udp::endpoint source;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    for (;;) {
        asio::error_code error;
        auto size = socket.receive_from(asio::buffer(data), source, 0, error);
        if (!error) { data.resize(size); return {std::move(data), source}; }
        check(error == asio::error::would_block || error == asio::error::try_again, "external UDP receive failed");
        check(std::chrono::steady_clock::now() < deadline, "external UDP receive timeout");
        std::this_thread::sleep_for(1ms);
    }
}
void udp() {
    Sink sink;
    net::Runtime runtime(sink, {.thread_count = 2});
    check(runtime.start().has_value(), "UDP runtime start");
    check(!runtime.udp_bind(0, "127.0.0.2", 0), "zero UDP owner accepted");
    check(!runtime.udp_bind(1, "bad host name", 0), "invalid UDP bind accepted");
    check(!runtime.udp_bind(1, "192.0.2.123", 0), "unavailable UDP address accepted");
    auto id = runtime.udp_bind(1, "127.0.0.2", 0);
    check(id.has_value(), "UDP bind");
    asio::io_context io;
    asio::ip::udp::socket peer(io, {asio::ip::make_address("127.0.0.2"), 0});
    const auto port = peer.local_endpoint().port();
    check(!runtime.udp_connect(1, *id, {"bad host name", port}), "invalid UDP target accepted");
    check(runtime.udp_connect(1, *id, {"localhost", port}).has_value(), "UDP connect");
    check(runtime.udp_connect(1, *id, {"127.0.0.2", port}).has_value(), "UDP target update");
    check(!runtime.udp_send(1, *id, net::Buffer(65508)), "oversize UDP accepted");
    check(!runtime.udp_send(1, *id, {}, {"127.0.0.2", 0}), "invalid UDP port accepted");
    std::uint64_t total = 0;
    asio::ip::udp::endpoint local;
    for (const auto& data : {net::Buffer{}, net::copy_buffer(std::string_view("a\0b", 3)), net::Buffer(65507, std::byte{42})}) {
        check(runtime.send(99, *id, data).has_value(), "default UDP cross-owner send");
        auto [bytes, source] = receive(peer);
        check(bytes == data, "UDP size or contents changed");
        local = source;
        peer.send_to(asio::buffer(data), source);
        auto event = sink.wait(net::EventType::udp, *id);
        check(event.payload == data && event.port == port && event.text == "127.0.0.2", "UDP event source/payload");
        total += data.size();
    }
    auto stat = info(runtime, *id);
    check(stat.type == net::SocketType::udp && stat.read == total && stat.written == total &&
        stat.owner == 1 && stat.queued_bytes == 0 && stat.reading && !stat.writing, "UDP statistics");
    check(stat.name == "127.0.0.2:" + std::to_string(port), "UDP peer info");
    check(runtime.pause_socket(1, *id).has_value(), "UDP pause");
    check(!info(runtime, *id).reading, "UDP pause info");
    check(runtime.start_socket(2, *id).has_value(), "UDP transfer");
    check(sink.wait(net::EventType::open, *id).destination == 2, "UDP transfer ack");
    runtime.close_all(1);
    check(info(runtime, *id).owner == 2, "former UDP owner closed transferred socket");
    peer.send_to(asio::buffer("next", 4), local);
    check(sink.wait(net::EventType::udp, *id).destination == 2, "UDP transfer routing");
    sink.reject_udp = true;
    peer.send_to(asio::buffer("reject", 6), local);
    sink.wait(net::EventType::close, *id);
    gone(runtime, *id);
    check(!runtime.udp_send(1, *id, {}), "closed UDP accepted send");
    runtime.join();
    check(runtime.info().empty(), "stopped runtime info");
}
void udp_budget_and_stop() {
    Sink sink;
    net::Runtime runtime(sink, {.write_warning_bytes = 2, .write_hard_limit = 4});
    check(runtime.start().has_value(), "budget start");
    auto id = runtime.udp_bind(1, "127.0.0.1", 0);
    check(id.has_value(), "budget UDP bind");
    check(runtime.udp_connect(1, *id, {"127.0.0.1", 9}).has_value(), "budget target");
    sink.block_open = true;
    Release release{sink};
    check(runtime.start_socket(2, *id).has_value(), "budget transfer");
    sink.wait(net::EventType::open, *id);
    for (int i = 0; i < 4; ++i) {
        check(runtime.udp_send(1, *id, net::Buffer(i < 2 ? 0 : 1)).has_value(), "UDP budget");
    }
    check(!runtime.udp_send(1, *id, {}), "empty UDP bypassed pre-post budget");
    runtime.close_all(1); // Ordered after the transfer; must not close it.
    sink.release();
    sink.wait(net::EventType::warning, *id);
    auto zero = sink.wait(net::EventType::warning, *id);
    while (zero.value) { zero = sink.wait(net::EventType::warning, *id); }
    check(info(runtime, *id).queued_bytes == 0, "UDP budget not released");
    runtime.close_all(2);
    sink.wait(net::EventType::close, *id);
    gone(runtime, *id);
    runtime.join();
    check(runtime.start().has_value(), "restart after UDP stop");
    check(runtime.info().empty(), "UDP remained after restart");
}
void fd_stream() {
    Sink sink;
    net::Runtime runtime(sink, {.thread_count = 2});
    check(runtime.start().has_value(), "FD runtime start");
    check(!runtime.bind_fd(1, -1), "invalid FD accepted");
    int pair[2];
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "socketpair");
    Fd caller{pair[0]}, peer{pair[1]};
    auto id = runtime.bind_fd(1, caller.value);
    check(id.has_value(), "socket FD bind");
    check(sink.wait(net::EventType::open, *id).text == "binding", "FD binding event");
    check(::write(peer.value, "hello", 5) == 5, "FD peer write");
    check(sink.wait(net::EventType::data, *id).payload == net::copy_buffer("hello"), "FD read");
    check(runtime.send(99, *id, net::copy_buffer("world")).has_value(), "FD cross-owner write");
    char data[5];
    check(::read(peer.value, data, sizeof(data)) == 5 && std::string_view(data, 5) == "world", "FD write contents");
    auto stat = info(runtime, *id);
    check(stat.type == net::SocketType::bound && stat.read == 5 && stat.written == 5, "FD info statistics");
    check(runtime.start_socket(2, *id).has_value(), "FD transfer");
    sink.wait(net::EventType::open, *id);
    runtime.close_all(1);
    check(info(runtime, *id).owner == 2, "FD transfer owner");
    runtime.close_all(2);
    sink.wait(net::EventType::close, *id);
    gone(runtime, *id);
    check(fcntl(caller.value, F_GETFD) >= 0, "FD close closed caller descriptor");
    check(::write(caller.value, "alive", 5) == 5 && ::read(peer.value, data, 5) == 5, "FD close shut down caller socket");
    int pipe_fds[2];
    check(pipe2(pipe_fds, O_CLOEXEC) == 0, "pipe");
    Fd input{pipe_fds[0]}, output{pipe_fds[1]};
    auto pipe_id = runtime.bind_fd(3, input.value);
    check(pipe_id.has_value(), "pipe FD bind");
    sink.wait(net::EventType::open, *pipe_id);
    check(::write(output.value, "stdin", 5) == 5, "pipe write");
    check(sink.wait(net::EventType::data, *pipe_id).payload == net::copy_buffer("stdin"), "pipe read");
    ::close(std::exchange(output.value, -1));
    sink.wait(net::EventType::close, *pipe_id); // EOF still leaves the borrowed fd owned by caller.
    check(runtime.close_socket(3, *pipe_id).has_value(), "pipe close");
    gone(runtime, *pipe_id);
    check(fcntl(input.value, F_GETFD) >= 0, "pipe close closed caller descriptor");
    // Cancel an in-flight write while its buffer is still referenced by Asio.
    id = runtime.bind_fd(4, caller.value);
    check(id.has_value(), "rebind FD");
    sink.wait(net::EventType::open, *id);
    check(runtime.send(4, *id, net::Buffer(4 * 1024 * 1024)).has_value(), "large FD write");
    check(runtime.shutdown_socket(4, *id).has_value(), "abort FD write");
    sink.wait(net::EventType::close, *id);
    // A broken bound pipe reports an error instead of delivering SIGPIPE to
    // the process. This test deliberately leaves the default SIGPIPE action.
    check(pipe2(pipe_fds, O_CLOEXEC) == 0, "broken pipe");
    Fd broken_reader{pipe_fds[0]}, broken_writer{pipe_fds[1]};
    auto broken = runtime.bind_fd(5, broken_writer.value);
    check(broken.has_value(), "bound pipe writer");
    sink.wait(net::EventType::open, *broken);
    check(!info(runtime, *broken).reading, "write-only pipe started reading");
    check(runtime.send(5, *broken, net::copy_buffer("pipe")).has_value(), "pipe send");
    check(::read(broken_reader.value, data, 4) == 4 && std::string_view(data, 4) == "pipe", "write-only pipe contents");
    ::close(std::exchange(broken_reader.value, -1));
    check(runtime.send(5, *broken, net::copy_buffer("broken")).has_value(), "broken pipe enqueue");
    sink.wait(net::EventType::error, *broken);
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0, "broken socketpair");
    Fd broken_socket{pair[0]}, broken_peer{pair[1]};
    auto broken_id = runtime.bind_fd(5, broken_socket.value);
    check(broken_id.has_value(), "broken socket bind");
    sink.wait(net::EventType::open, *broken_id);
    ::close(std::exchange(broken_peer.value, -1));
    sink.wait(net::EventType::close, *broken_id);
    check(runtime.send(5, *broken_id, net::copy_buffer("broken")).has_value(), "broken socket enqueue");
    sink.wait(net::EventType::error, *broken_id);
    runtime.join();
    check(fcntl(caller.value, F_GETFD) >= 0, "runtime shutdown closed caller descriptor");
}
void info_stop_race() {
    Sink sink;
    net::Runtime runtime(sink, {.thread_count = 2});
    check(runtime.start().has_value(), "race start");
    for (int i = 0; i < 16; ++i) { check(runtime.udp_bind(1, "127.0.0.1", 0).has_value(), "race bind"); }
    std::jthread reader([&] { for (int i = 0; i < 50; ++i) { (void)runtime.info(); } });
    runtime.request_stop();
    runtime.join();
}
}  // namespace
int main() {
    try { udp(); udp_budget_and_stop(); fd_stream(); info_stop_race(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
