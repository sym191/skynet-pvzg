#include "skynet/network/runtime.hpp"

#include <asio.hpp>
#include <algorithm>
#include <array>
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
        const bool reject = (reject_accept.load() && event.type == net::EventType::accept) ||
            (reject_data.load() && event.type == net::EventType::data);
        {
            std::unique_lock lock(mutex_);
            const bool block = event.type == net::EventType::open && block_next_open.exchange(false);
            events_.push_back(std::move(event));
            if (block) {
                blocked_ = true;
                ready_.notify_all();
                ready_.wait(lock, [&] { return !blocked_; });
            }
        }
        ready_.notify_all();
        return !reject;
    }
    net::Event wait(net::EventType type, net::SocketId socket) {
        std::unique_lock lock(mutex_);
        auto find = [&] { return std::ranges::find_if(events_, [&](const auto& event) {
            return event.type == type && event.socket == socket;
        }); };
        if (!ready_.wait_for(lock, 4s, [&] { return find() != events_.end(); })) {
            for (const auto& event : events_) {
                std::cerr << "queued event " << int(event.type) << " socket " << event.socket << ": " << event.text << '\n';
            }
            throw std::runtime_error("network event timed out: type=" + std::to_string(int(type)) + " socket=" + std::to_string(socket));
        }
        auto found = find();
        auto result = std::move(*found);
        events_.erase(found);
        return result;
    }
    std::atomic<bool> reject_accept{false};
    std::atomic<bool> reject_data{false};
    std::atomic<bool> block_next_open{false};
    std::atomic<net::ServiceId> retired_owner{0};
    bool owner_alive(net::ServiceId owner) const noexcept override {
        return owner != retired_owner.load();
    }
    void release() {
        std::lock_guard lock(mutex_);
        blocked_ = false;
        ready_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<net::Event> events_;
    bool blocked_{false};
};
struct ReleaseGuard {
    Sink& sink;
    ~ReleaseGuard() { sink.release(); }
};

void queued_operations() {
    Sink sink;
    net::Runtime runtime(sink, {.thread_count = 1, .write_warning_bytes = 8192,
        .write_hard_limit = 32768});
    check(runtime.start().has_value(), "budget runtime start");
    auto listener = runtime.listen(1, "127.0.0.1", 0);
    check(listener.has_value(), "budget listener");
    const auto port = sink.wait(net::EventType::open, *listener).port;
    check(runtime.start_socket(1, *listener).has_value(), "budget listener start");
    sink.wait(net::EventType::open, *listener);
    asio::io_context io;
    const asio::ip::tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), port);
    for (int scenario = 0; scenario != 4; ++scenario) {
        asio::ip::tcp::socket client(io);
        client.connect(endpoint);
        const auto accepted = sink.wait(net::EventType::accept, *listener).accepted_socket;
        sink.block_next_open.store(true);
        check(runtime.start_socket(2, accepted).has_value(), "blocked start");
        ReleaseGuard release{sink};
        sink.wait(net::EventType::open, accepted);
        if (scenario == 0) {
            // The sole executor thread is blocked: all four buffers live in
            // post closures, and priority ordering can be checked deterministically.
            for (auto [byte, priority] : {std::pair{'a', net::SendPriority::low},
                    {'b', net::SendPriority::low}, {'c', net::SendPriority::high},
                    {'d', net::SendPriority::high}}) {
                check(runtime.send(9, accepted, net::Buffer(8192, std::byte(byte)), priority).has_value(), "queued priority write");
            }
            sink.release();
            net::Buffer received(32768);
            asio::read(client, asio::buffer(received));
            const std::string_view order = "acdb";
            for (std::size_t i = 0; i != received.size(); ++i) {
                check(received[i] == std::byte(order[i / 8192]), "priority reordered an in-flight buffer");
            }
            check(runtime.shutdown_socket(9, accepted).has_value(), "priority shutdown");
        } else if (scenario == 1) {
            check(runtime.send(9, accepted, net::Buffer(24576)).has_value(), "first reserved write");
            auto rejected = runtime.send(9, accepted, net::Buffer(16384));
            check(!rejected && rejected.error().code == net::ErrorCode::socket_limit_reached,
                "executor closures bypassed the hard limit");
            sink.release();
            sink.wait(net::EventType::error, accepted);
        } else if (scenario == 2) {
            check(runtime.start_socket(3, accepted).has_value(), "queued transfer");
            runtime.close_all(3); // owner_snapshot is still 2 until the strand resumes.
            sink.release();
            check(sink.wait(net::EventType::open, accepted).destination == 3, "transfer destination");
        } else {
            check(runtime.start_socket(3, accepted).has_value(), "retired transfer request");
            sink.retired_owner.store(3);
            runtime.close_all(3);
            sink.release();
            check(!runtime.listen(3, "127.0.0.1", 0), "retired owner opened a listener");
        }
        sink.wait(net::EventType::close, accepted);
    }
    runtime.join();
}
}  // namespace

int main() {
    try {
        queued_operations();
        Sink sink;
        net::Runtime runtime(sink, {.thread_count = 2, .write_hard_limit = 16 * 1024 * 1024});
        check(runtime.start().has_value(), "network start failed");
        auto listener = runtime.listen(1, "127.0.0.1", 0);
        check(listener.has_value(), "listen failed");
        auto listening = sink.wait(net::EventType::open, *listener);
        check(listening.port != 0, "listen did not return port");
        check(runtime.start_socket(1, *listener).has_value(), "listener start failed");
        check(sink.wait(net::EventType::open, *listener).text == "start", "missing listener start acknowledgement");
        asio::io_context io;
        const asio::ip::tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), listening.port);
        for (int iteration = 0; iteration != 12; ++iteration) {
            asio::ip::tcp::socket client(io);
            client.connect(endpoint);
            const auto accepted = sink.wait(net::EventType::accept, *listener).accepted_socket;
            check(runtime.start_socket(2, accepted).has_value(), "accepted start failed");
            auto started = sink.wait(net::EventType::open, accepted);
            check(started.destination == 2 && started.text == "start", "wrong start owner");
            check(runtime.start_socket(3, accepted).has_value(), "transfer failed");
            auto transfer = sink.wait(net::EventType::open, accepted);
            check(transfer.destination == 3 && transfer.text == "transfer", "missing transfer acknowledgement");
            runtime.close_all(2); // Must not close the new owner's connection.
            check(runtime.send(3, accepted, net::Buffer(8 * 1024 * 1024, std::byte{42})).has_value(), "send failed");
            if (iteration % 2 == 0) {
                check(runtime.shutdown_socket(3, accepted).has_value(), "shutdown failed");
            } else {
                // The second large write crosses the hard limit while the first is in flight.
                check(!runtime.send(3, accepted, net::Buffer(16 * 1024 * 1024, std::byte{42})), "oversized send accepted");
            }
            sink.wait(net::EventType::close, accepted);
        }
        sink.reject_accept.store(true);
        {
            asio::ip::tcp::socket client(io);
            client.connect(endpoint);
            const auto rejected = sink.wait(net::EventType::accept, *listener).accepted_socket;
            sink.wait(net::EventType::close, rejected);
        }
        sink.reject_accept.store(false);
        // EOF closes only the read half. A response remains writable, and a
        // graceful close preserves the tail even with a slow reader.
        {
            asio::ip::tcp::socket client(io);
            client.connect(endpoint);
            const auto accepted = sink.wait(net::EventType::accept, *listener).accepted_socket;
            check(runtime.start_socket(2, accepted).has_value(), "start half-close test");
            sink.wait(net::EventType::open, accepted);
            asio::write(client, asio::buffer("half", 4));
            check(sink.wait(net::EventType::data, accepted).payload == net::copy_buffer("half"), "lost half-close data");
            client.shutdown(asio::ip::tcp::socket::shutdown_send);
            sink.wait(net::EventType::close, accepted);
            check(runtime.send(99, accepted, net::copy_buffer("response")).has_value(), "write after EOF failed");
            check(runtime.close_socket(99, accepted).has_value(), "cross-service close failed");
            std::array<char, 8> response{};
            asio::read(client, asio::buffer(response));
            check(std::string_view(response.data(), response.size()) == "response", "lost EOF response");
            asio::error_code error;
            client.read_some(asio::buffer(response), error);
            check(error == asio::error::eof, "graceful close did not drain");
        }
        {
            asio::ip::tcp::socket client(io);
            client.connect(endpoint);
            const auto accepted = sink.wait(net::EventType::accept, *listener).accepted_socket;
            check(runtime.start_socket(2, accepted).has_value(), "start tail test");
            sink.wait(net::EventType::open, accepted);
            net::Buffer tail(4 * 1024 * 1024, std::byte{17});
            check(runtime.send(99, accepted, tail, net::SendPriority::low).has_value(), "tail write failed");
            auto warning = sink.wait(net::EventType::warning, accepted);
            check(warning.value > 0, "warning is not in KiB");
            check(runtime.close_socket(99, accepted).has_value(), "tail close failed");
            // CLOSE is delivered before this client starts draining the socket.
            sink.wait(net::EventType::close, accepted);
            net::Buffer received(tail.size());
            asio::read(client, asio::buffer(received));
            check(received == tail, "graceful close lost its tail");
            check(sink.wait(net::EventType::warning, accepted).value == 0, "missing warning reset");
        }
        sink.reject_data.store(true);
        {
            asio::ip::tcp::socket client(io);
            client.connect(endpoint);
            const auto accepted = sink.wait(net::EventType::accept, *listener).accepted_socket;
            check(runtime.start_socket(2, accepted).has_value(), "start data test");
            sink.wait(net::EventType::open, accepted);
            asio::write(client, asio::buffer("drop", 4));
            sink.wait(net::EventType::close, accepted);
        }
        runtime.request_stop();
        runtime.join();
        for (int iteration = 0; iteration != 16; ++iteration) {
            check(runtime.start().has_value(), "network restart failed");
            auto socket = runtime.listen(1, "127.0.0.1", 0);
            check(socket.has_value(), "restart listen failed");
            sink.wait(net::EventType::open, *socket);
            std::jthread producer([&] {
                while (runtime.running()) {
                    (void)runtime.start_socket(1, *socket);
                    runtime.close_all(99);
                    std::this_thread::yield();
                }
            });
            std::jthread stopper([&] { runtime.request_stop(); });
            runtime.join();
            producer.join();
            stopper.join();
            check(!runtime.start_socket(1, *socket), "stopped runtime accepted socket operation");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
