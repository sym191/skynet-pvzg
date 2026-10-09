#include "Context.hpp"
#include "Runtime.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace skynet::runtime {
struct RuntimeTestAccess {
    static std::shared_ptr<Context> context(Runtime& runtime, Handle handle) {
        return runtime.handles_.grab(handle);
    }
    static bool endless(const Context& context) {
        return context.endless_.load(std::memory_order_acquire);
    }
    static bool idle(const Runtime& runtime) {
        for (std::size_t index = 0; index < runtime.options_.worker_count; ++index) {
            if (runtime.monitors_[index].destination != 0) {
                return false;
            }
        }
        return true;
    }
    static void stop_monitor(Runtime& runtime) {
        runtime.monitor_worker_.request_stop();
        runtime.monitor_worker_.join();
    }
    static void check_workers(Runtime& runtime) {
        for (std::size_t index = 0; index < runtime.options_.worker_count; ++index) {
            runtime.check(index);
        }
    }
};
}  // namespace skynet::runtime

namespace {
namespace rt = skynet::runtime;
using namespace std::chrono_literals;

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

template <class Predicate>
bool wait_until(Predicate predicate, std::chrono::steady_clock::duration timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

struct State {
    std::mutex mutex;
    std::condition_variable ready;
    bool entered{false};
    bool released{false};
    bool throw_after_release{false};
    std::atomic<bool> peer_running{true};
    std::atomic<bool> peer_done{false};
    std::atomic<int> peer_messages{0};
};

class Blocked final : public rt::Service {
public:
    explicit Blocked(State& state) : state_(state) {}

    void dispatch(rt::Context&, rt::Message) override {
        std::unique_lock lock(state_.mutex);
        state_.entered = true;
        state_.ready.notify_all();
        state_.ready.wait(lock, [this] { return state_.released; });
        if (state_.throw_after_release) {
            throw std::runtime_error("monitor test dispatch failure");
        }
    }

    void signal(rt::Context&, int) override {
        // Also unblock failed tests so Runtime destruction can join the worker.
        std::lock_guard lock(state_.mutex);
        state_.released = true;
        state_.ready.notify_all();
    }

private:
    State& state_;
};

class Progressing final : public rt::Service {
public:
    explicit Progressing(State& state) : state_(state) {}

    void dispatch(rt::Context& context, rt::Message) override {
        state_.peer_messages.fetch_add(1);
        if (state_.peer_running.load()) {
            std::this_thread::sleep_for(5ms);
            (void)context.send(context.handle(), 0, 0, {});
        } else {
            state_.peer_done.store(true);
        }
    }

private:
    State& state_;
};

void run() {
    State state;
    rt::Runtime runtime({.worker_count = 2, .harbor_id = 0, .profile = false});
    check(runtime.services().register_factory("blocked", [&] {
        return std::make_unique<Blocked>(state);
    }), "blocked factory");
    check(runtime.services().register_factory("progressing", [&] {
        return std::make_unique<Progressing>(state);
    }), "progressing factory");
    check(runtime.start().has_value(), "runtime start");
    const auto blocked = runtime.launch("blocked");
    const auto peer = runtime.launch("progressing");
    check(blocked.has_value() && peer.has_value(), "launch services");
    const auto blocked_context = rt::RuntimeTestAccess::context(runtime, *blocked);
    const auto peer_context = rt::RuntimeTestAccess::context(runtime, *peer);
    check(runtime.send(*blocked, rt::Message::text(*peer, 0, "block")), "send block");
    {
        std::unique_lock lock(state.mutex);
        check(state.ready.wait_for(lock, 2s, [&] { return state.entered; }), "dispatch did not start");
    }
    check(runtime.send(*peer, rt::Message::text(0, 0, "progress")), "send progress");
    check(wait_until([&] { return state.peer_messages.load() >= 5; }), "other worker did not progress");
    check(wait_until([&] { return rt::RuntimeTestAccess::endless(*blocked_context); }, 6s),
          "progress on another worker masked a blocked dispatch");
    check(!peer_context->consume_endless(), "progressing worker was marked endless");
    check(blocked_context->command("STAT", "endless") == "1" &&
          blocked_context->command("STAT", "endless") == "0", "STAT endless must consume the flag");

    rt::RuntimeTestAccess::stop_monitor(runtime);
    (void)blocked_context->consume_endless();
    state.peer_running.store(false);
    {
        std::lock_guard lock(state.mutex);
        state.released = true;
        state.ready.notify_all();
    }
    check(wait_until([&] {
        return state.peer_done.load() && rt::RuntimeTestAccess::idle(runtime);
    }), "dispatch did not clear worker state");
    rt::RuntimeTestAccess::check_workers(runtime);
    rt::RuntimeTestAccess::check_workers(runtime);
    check(!blocked_context->consume_endless() && !peer_context->consume_endless(), "idle worker was marked endless");

    {
        std::lock_guard lock(state.mutex);
        state.entered = false;
        state.released = false;
        state.throw_after_release = true;
    }
    check(runtime.send(*blocked, rt::Message::text(*peer, 0, "throw")), "send throwing dispatch");
    {
        std::unique_lock lock(state.mutex);
        check(state.ready.wait_for(lock, 2s, [&] { return state.entered; }), "throwing dispatch did not start");
    }
    rt::RuntimeTestAccess::check_workers(runtime);
    check(!blocked_context->consume_endless(), "new dispatch inherited an old monitoring sample");
    rt::RuntimeTestAccess::check_workers(runtime);
    check(blocked_context->consume_endless(), "unchanged dispatch was not detected");
    {
        std::lock_guard lock(state.mutex);
        state.released = true;
        state.ready.notify_all();
    }
    check(wait_until([&] { return rt::RuntimeTestAccess::idle(runtime); }), "exception left worker state busy");
    rt::RuntimeTestAccess::check_workers(runtime);
    rt::RuntimeTestAccess::check_workers(runtime);
    check(!blocked_context->consume_endless(), "exception caused a stale endless report");
    runtime.request_stop();
    runtime.join();

    check(runtime.start().has_value(), "runtime restart");
    rt::RuntimeTestAccess::stop_monitor(runtime);
    rt::RuntimeTestAccess::check_workers(runtime);
    rt::RuntimeTestAccess::check_workers(runtime);
    check(rt::RuntimeTestAccess::idle(runtime), "restart retained worker state");
    runtime.request_stop();
    runtime.join();
}

void check_stop_latency() {
    rt::Runtime runtime({.worker_count = 8, .harbor_id = 0});
    check(runtime.start().has_value(), "stop-latency runtime start");
    std::this_thread::sleep_for(50ms);
    const auto started = std::chrono::steady_clock::now();
    runtime.request_stop();
    runtime.join();
    check(std::chrono::steady_clock::now() - started < 2s,
          "stopping waited for the full worker-count scan interval");
}
}  // namespace

int main() {
    try {
        run();
        check_stop_latency();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
