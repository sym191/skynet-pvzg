#include "skynet/network/runtime.hpp"

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
        const bool reject = (reject_accept.load() && event.type == net::EventType::accept) ||
            (reject_data.load() && event.type == net::EventType::data);
        {
            std::lock_guard lock(mutex_);
            events_.push_back(std::move(event));
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
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<net::Event> events_;
};
}  // namespace

int main() {
    try {
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
                check(runtime.send(3, accepted, net::Buffer(16 * 1024 * 1024, std::byte{42})).has_value(), "second send");
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
