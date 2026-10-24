#include "Context.hpp"
#include "Runtime.hpp"
#include "Service.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

extern "C" {
#include "skynet.h"
#include "skynet_socket.h"
}

namespace {
namespace rt = skynet::runtime;
using namespace std::chrono_literals;
void check(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
struct State {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<rt::Message> messages;
    double cpu{-1};
    double time{-1};
    void* reserved{nullptr};
    bool empty_owned{false};
};
class Probe final : public rt::Service {
public:
    explicit Probe(State& state) : state_(state) {}
    bool init(rt::Context& context, std::string_view) override {
        check(context.send(".absent", 0, 0, {}, 0, true) == -1, "missing name was accepted");
        check(context.new_session() == 1, "rejected named send allocated a session");
        auto* native = reinterpret_cast<skynet_context*>(&context);
        check(skynet_current_handle() == context.handle(), "init TLS handle is missing");
        // Facade rejection must consume MEMORY even before validating its target.
        const char udp_address[7] = {1, 4, static_cast<char>(210), 127, 0, 0, 2};
        socket_sendbuffer rejected{0x7fffffff, SOCKET_BUFFER_MEMORY, std::malloc(8), 8};
        check(rejected.buffer && skynet_socket_udp_sendbuffer(native, udp_address, &rejected) == -1,
            "UDP facade rejection ownership");
        rejected = {0x7fffffff, SOCKET_BUFFER_MEMORY, std::malloc(8), 8};
        check(rejected.buffer && skynet_socket_udp_sendbuffer(native, nullptr, &rejected) == -1,
            "UDP invalid-address rejection ownership");
        int address_size = -1;
        skynet_socket_message not_udp{SKYNET_SOCKET_TYPE_DATA, 1, 0, nullptr};
        check(!skynet_socket_udp_address(&not_udp, &address_size) && address_size == 0,
            "UDP address accepted non-UDP message");
        check(skynet_sendname(native, 0, ".absent", PTYPE_TAG_ALLOCSESSION, 0, nullptr, 0) == -1,
              "C sendname did not reject missing destination");
        check(context.new_session() == 2, "C sendname failure consumed a session");
        auto* owned = std::malloc(1);
        check(skynet_send(native, 0, context.handle(), PTYPE_TAG_DONTCOPY, 0, owned,
              (SIZE_MAX >> 8) + 1) == -2, "oversize must return -2 and release payload");
        check(context.command("REG", ".probe") == "probe", "REG return differs");
        check(!context.command("REG", "GLOBAL"), "REG accepted global name");
        const auto address = *context.command("REG");
        check(context.command("QUERY", ".probe") == address, "QUERY missing local name");
        check(!context.command("QUERY", address), "QUERY accepted numeric address");
        check(context.command("NAME", ".alias " + address) == "alias", "NAME return differs");
        check(!context.command("NAME", ".bad .probe"), "NAME accepted alias target");
        (void)context.command("LOGON", address);
        check(context.message_logging(), "LOGON failed to open per-service log");
        skynet_callback(native, &state_, [](skynet_context*, void* user, int type, int, uint32_t,
                                          const void* data, size_t size) {
            if (type == 253) {
                static_cast<State*>(user)->empty_owned = data != nullptr && size == 0;
                return 0;
            }
            static_cast<State*>(user)->reserved = const_cast<void*>(data);
            return 1;
        });
        auto* empty = std::malloc(1);
        check(empty != nullptr, "malloc empty message");
        check(skynet_send(native, 0, context.handle(), 253 | PTYPE_TAG_DONTCOPY,
                          0, empty, 0) == 0, "non-null zero-size message send");
        return true;
    }
    void dispatch(rt::Context& context, rt::Message message) override {
        if (message.type == 253 || message.type == 254) {
            context.dispatch_native(std::move(message));
            return;
        }
        if (message.string_payload() == "sleep") {
            std::this_thread::sleep_for(80ms);
            state_.time = std::stod(*context.command("STAT", "time"));
        }
        if (message.string_payload() == "barrier") {
            state_.cpu = std::stod(*context.command("STAT", "cpu"));
            (void)context.command("LOGOFF", *context.command("REG"));
        }
        {
            std::lock_guard lock(state_.mutex);
            state_.messages.push_back(std::move(message));
        }
        state_.ready.notify_all();
    }
private:
    State& state_;
};
class Failed final : public rt::Service {
public:
    explicit Failed(rt::Handle source) : source_(source) {}
    bool init(rt::Context& context, std::string_view) override {
        check(context.send(context.handle(), 10, 42, {}, source_) == 42, "init cannot enqueue");
        return false;
    }
    void dispatch(rt::Context&, rt::Message) override {}
private:
    rt::Handle source_;
};

void run(bool profile, const std::filesystem::path& directory) {
    State state;
    rt::Runtime runtime({.worker_count = 4, .harbor_id = 0, .profile = profile});
    check(runtime.environment().set("logpath", directory.string()), "logpath failed");
    check(runtime.services().register_factory("probe", [&] { return std::make_unique<Probe>(state); }), "factory");
    check(runtime.start().has_value(), "start");
    const auto target = runtime.launch("probe");
    check(target.has_value(), "launch probe");
    check(runtime.services().register_factory("failed", [&] { return std::make_unique<Failed>(*target); }), "factory");
    check(!runtime.launch("failed"), "init failure returned a live handle");
    check(runtime.service_count() == 1, "init failure leaked service");
    check(runtime.send(*target, rt::Message::text(0, 0, "sleep")), "send sleep");
    auto* owned = std::malloc(4);
    check(owned != nullptr, "malloc");
    check(runtime.send(*target, rt::Message{.type = 254, .data = rt::MessageBuffer::adopt(owned, 4)}), "send retained");
    check(runtime.send(*target, rt::Message::text(0, 0, "barrier")), "send barrier");
    {
        std::unique_lock lock(state.mutex);
        check(state.ready.wait_for(lock, 3s, [&] { return state.messages.size() == 3; }), "missing drop error/barrier");
        check(state.messages[0].type == 7 && state.messages[0].session == 42 && state.messages[0].source != 0,
              "failed init did not reply with original session and failed source");
        check(state.reserved == owned, "callback reservation changed pointer ownership");
        check(state.empty_owned, "non-null zero-size DONTCOPY pointer was lost");
        std::free(state.reserved);
        check(state.time >= 0 && state.time < 0.04 && state.cpu >= 0 && state.cpu < 0.04,
              "STAT counted sleeping as CPU time");
        if (!profile) { check(state.time == 0 && state.cpu == 0, "profile=false should report zero"); }
    }
    (void)runtime.retire(*target);
    runtime.wait();
    check(skynet_current_handle() == 0, "worker TLS leaked to caller");
    check(runtime.start().has_value(), "runtime could not restart after join");
    runtime.request_stop();
    runtime.join();
}
}  // namespace

int main() {
    char pattern[] = "/tmp/skpvzg-contract-XXXXXX";
    const char* directory = mkdtemp(pattern);
    if (!directory) { return 1; }
    try {
        run(true, directory);
        run(false, directory);
        std::ifstream log(std::filesystem::path(directory) / "00000001.log");
        const std::string content((std::istreambuf_iterator<char>(log)), {});
        check(content.find("736c656570") != std::string::npos && content.find("close time:") != std::string::npos,
              "message log omitted payload or LOGOFF record");
        std::filesystem::remove_all(directory);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(directory);
        return 1;
    }
}
