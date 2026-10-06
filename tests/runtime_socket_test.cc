#include "Context.hpp"
#include "Runtime.hpp"
#include "SocketEvent.hpp"

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

extern "C" {
#include "skynet_socket.h"
}

namespace {
namespace rt = skynet::runtime;
namespace net = skynet::network;
using namespace std::chrono_literals;
void check(bool result, const char* message) {
    if (!result) { throw std::runtime_error(message); }
}
struct Observation {
    int type;
    int id;
    int value;
    std::string data;
};
struct State {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<Observation> events;
    std::atomic<rt::Context*> context{nullptr};
    bool blocked{false};
    bool released{false};
    std::atomic<bool> wrong_thread{false};
    Observation wait(int type, int id) {
        std::unique_lock lock(mutex);
        const auto find = [&] { return std::ranges::find_if(events, [&](const auto& event) {
            return event.type == type && event.id == id;
        }); };
        check(ready.wait_for(lock, 4s, [&] { return find() != events.end(); }), "actor socket event timeout");
        auto result = std::move(*find());
        events.erase(find());
        return result;
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        ready.notify_all();
    }
};
class Probe final : public rt::Service {
public:
    explicit Probe(std::shared_ptr<State> state) : state_(std::move(state)) {}
    bool init(rt::Context& context, std::string_view) override {
        state_->context.store(&context);
        return true;
    }
    void dispatch(rt::Context& context, rt::Message message) override {
        if (rt::Context::current() != &context) { state_->wrong_thread.store(true); }
        std::unique_lock lock(state_->mutex);
        if (message.type == 0) {
            state_->blocked = true;
            state_->ready.notify_all();
            state_->ready.wait(lock, [&] { return state_->released; });
            return;
        }
        check(message.type == 6 && message.source == 0 && message.session == 0, "socket metadata");
        const auto* socket = reinterpret_cast<const skynet_socket_message*>(message.data.data());
        const auto* bytes = socket->buffer ? socket->buffer : reinterpret_cast<const char*>(socket + 1);
        const auto size = socket->buffer ? static_cast<std::size_t>(socket->ud) :
            message.data.size() - sizeof(*socket);
        state_->events.push_back({socket->type, socket->id, socket->ud, std::string(bytes, size)});
        state_->ready.notify_all();
        // This C++ consumer never hands off the nested data. Message destruction
        // must reclaim both allocations, just as a dropped mailbox does.
    }
private:
    std::shared_ptr<State> state_;
};
struct ReleaseGuard {
    State& state;
    ~ReleaseGuard() { state.release(); }
};

void wait_eof(asio::ip::tcp::socket& client) {
    client.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    char byte;
    while (std::chrono::steady_clock::now() < deadline) {
        asio::error_code error;
        client.read_some(asio::buffer(&byte, 1), error);
        if (error == asio::error::eof || error == asio::error::connection_reset) { return; }
        check(error == asio::error::would_block || error == asio::error::try_again, "unexpected retirement read");
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error("service retirement left its socket open");
}
}  // namespace

int main() {
    try {
        rt::Runtime runtime({.worker_count = 2, .harbor_id = 0});
        auto first = std::make_shared<State>();
        auto second = std::make_shared<State>();
        check(runtime.services().register_factory("first", [first] { return std::make_unique<Probe>(first); }), "first factory");
        check(runtime.services().register_factory("second", [second] { return std::make_unique<Probe>(second); }), "second factory");
        check(runtime.start().has_value(), "actor start");
        const auto owner = runtime.launch("first");
        const auto receiver = runtime.launch("second");
        check(owner.has_value() && receiver.has_value(), "actor launch");
        const auto listener = runtime.sockets().listen(*owner, "127.0.0.1", 0);
        check(listener.has_value(), "actor listen");
        const auto listening = first->wait(2, *listener);
        check(listening.value > 0 && listening.data == "127.0.0.1", "listen-ready ABI");
        check(runtime.sockets().start_socket(*owner, *listener).has_value(), "actor listener start");
        check(first->wait(2, *listener).data == "start", "start-ready ABI");
        asio::io_context io;
        asio::ip::tcp::socket client(io);
        client.connect({asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(listening.value)});
        const auto accepted = first->wait(4, *listener).value;
        check(accepted > 0, "accepted id ABI");
        check(runtime.sockets().start_socket(*receiver, accepted).has_value(), "actor transfer");
        check(second->wait(2, accepted).data == "start", "accepted start ABI");
        check(runtime.retire(*owner), "retire former owner");
        asio::write(client, asio::buffer("a\0b", 3));
        const auto data = second->wait(1, accepted);
        check(data.value == 3 && data.data == std::string("a\0b", 3), "data ABI / transfer lifetime");

        // Retire with a callback in flight and nested buffers queued behind it.
        check(runtime.send(*receiver, rt::Message::text(0, 0, "block")), "block actor");
        ReleaseGuard release{*second};
        {
            std::unique_lock lock(second->mutex);
            check(second->ready.wait_for(lock, 4s, [&] { return second->blocked; }), "actor did not block");
        }
        asio::write(client, asio::buffer("queued", 6));
        const auto deadline = std::chrono::steady_clock::now() + 4s;
        while (second->context.load()->queue_length() == 0) {
            check(std::chrono::steady_clock::now() < deadline, "socket message did not queue");
            std::this_thread::sleep_for(1ms);
        }
        check(runtime.retire(*receiver), "retire blocked owner");
        check(!runtime.sockets().listen(*receiver, "127.0.0.1", 0), "dead actor opened socket");
        wait_eof(client);
        second->release();
        check(!first->wrong_thread.load() && !second->wrong_thread.load(), "socket callback escaped actor worker");
        // Rejected publication must reclaim nested storage as well.
        auto rejected = rt::Message{.type = 6, .data = rt::make_socket_message(net::Event{
            .type = net::EventType::data, .destination = *receiver, .socket = accepted,
            .payload = net::copy_buffer("rejected"), .text = {}})};
        check(!runtime.send(*receiver, std::move(rejected)), "dead actor accepted event");
        runtime.join();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
