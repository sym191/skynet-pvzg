#include "Context.hpp"
#include "Runtime.hpp"
#include "Service.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace runtime = skynet::runtime;

struct ProbeState {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<runtime::Message> messages;
    std::atomic<int> active_dispatches{0};
    std::atomic<int> maximum_dispatches{0};
    std::atomic<int> signal_count{0};
    runtime::Handle primary_handle{0};
    int timeout_session{0};
    bool commands_ok{false};
};

void update_maximum(std::atomic<int>& maximum, int value) {
    int current = maximum.load(std::memory_order_relaxed);
    while (current < value && !maximum.compare_exchange_weak(
            current, value, std::memory_order_relaxed)) {
    }
}

class ProbeService final : public runtime::Service {
public:
    explicit ProbeService(std::shared_ptr<ProbeState> state)
        : state_(std::move(state)) {}

    bool init(runtime::Context& context, std::string_view arguments) override {
        if (arguments == "primary") {
            state_->primary_handle = context.handle();
            const auto timeout = context.command("TIMEOUT", "2");
            if (!timeout) {
                return false;
            }
            state_->timeout_session = std::stoi(*timeout);
        }
        return true;
    }

    void dispatch(runtime::Context& context, runtime::Message message) override {
        const int active = state_->active_dispatches.fetch_add(1) + 1;
        update_maximum(state_->maximum_dispatches, active);
        std::this_thread::sleep_for(50us);

        const std::string payload = message.string_payload();
        if (payload == "exercise-commands") {
            const auto reg = context.command("REG");
            const auto query = context.command("QUERY", ".probe");
            const auto marker = context.command("GETENV", "marker");
            const auto set_dynamic = context.command("SETENV", "dynamic value");
            const auto dynamic = context.command("GETENV", "dynamic");
            const auto start_time = context.command("STARTTIME");
            const auto stat_message = context.command("STAT", "message");
            const auto named = context.command("NAME", ".probe_alias " + *reg);
            const auto alias = context.command("QUERY", ".probe_alias");
            const auto launched = context.command("LAUNCH", "probe child");
            const auto monitor_set = context.command("MONITOR", *reg);
            const auto monitor_get = context.command("MONITOR");
            const auto signal = context.command("SIGNAL", *reg + " 0x7");
            const auto log_on = context.command("LOGON", *reg);
            const auto log_off = context.command("LOGOFF", *reg);
            (void)set_dynamic;
            (void)monitor_set;
            (void)signal;
            (void)log_on;
            (void)log_off;
            if (launched) {
                (void)context.command("KILL", *launched);
            }
            state_->commands_ok = reg && query && marker && dynamic && start_time &&
                stat_message && named && alias &&
                monitor_get && *reg == *query && *marker == "available" &&
                *dynamic == "value" && *named == "probe_alias" &&
                *alias == *reg && *monitor_get == *reg &&
                std::stoul(*start_time) > 0 && std::stoull(*stat_message) > 0;
        } else if (payload == "exit") {
            (void)context.command("EXIT");
        } else if (payload == "abort") {
            (void)context.command("ABORT");
        }

        {
            std::lock_guard lock(state_->mutex);
            state_->messages.push_back(std::move(message));
        }
        state_->active_dispatches.fetch_sub(1);
        state_->ready.notify_all();
    }

    void signal(runtime::Context&, int signal) override {
        if (signal == 7) {
            state_->signal_count.fetch_add(1);
        }
    }

private:
    std::shared_ptr<ProbeState> state_;
};

class ExitDuringInitService final : public runtime::Service {
public:
    bool init(runtime::Context& context, std::string_view) override {
        (void)context.command("EXIT");
        return true;
    }

    void dispatch(runtime::Context&, runtime::Message) override {}
};

struct BlockingState {
    std::mutex mutex;
    std::condition_variable ready;
    bool started{false};
    bool release{false};
};

class BlockingService final : public runtime::Service {
public:
    explicit BlockingService(std::shared_ptr<BlockingState> state)
        : state_(std::move(state)) {}

    void dispatch(runtime::Context&, runtime::Message) override {
        std::unique_lock lock(state_->mutex);
        state_->started = true;
        state_->ready.notify_all();
        state_->ready.wait(lock, [this] { return state_->release; });
    }

private:
    std::shared_ptr<BlockingState> state_;
};

struct ObserverState {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<runtime::Message> messages;
};

class ObserverService final : public runtime::Service {
public:
    explicit ObserverService(std::shared_ptr<ObserverState> state)
        : state_(std::move(state)) {}

    void dispatch(runtime::Context&, runtime::Message message) override {
        {
            std::lock_guard lock(state_->mutex);
            state_->messages.push_back(std::move(message));
        }
        state_->ready.notify_all();
    }

private:
    std::shared_ptr<ObserverState> state_;
};

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

bool wait_for_messages(
    const std::shared_ptr<ProbeState>& state,
    std::size_t count,
    std::chrono::milliseconds timeout = 5s
) {
    std::unique_lock lock(state->mutex);
    return state->ready.wait_for(lock, timeout, [&] {
        return state->messages.size() >= count;
    });
}

template <class Predicate>
bool wait_for_message(
    const std::shared_ptr<ProbeState>& state,
    Predicate predicate,
    std::chrono::milliseconds timeout = 5s
) {
    std::unique_lock lock(state->mutex);
    return state->ready.wait_for(lock, timeout, [&] {
        return std::ranges::any_of(state->messages, predicate);
    });
}

bool wait_for_service_count(
    runtime::Runtime& runtime,
    std::size_t count,
    std::chrono::milliseconds timeout = 5s
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (runtime.service_count() == count) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

}  // namespace

int main() {
    auto state = std::make_shared<ProbeState>();
    runtime::Runtime runtime(runtime::RuntimeOptions{
        .worker_count = 4,
        .harbor_id = 1,
        .profile = true,
    });
    if (!runtime.environment().set("marker", "available")) {
        return fail("failed to initialize runtime environment");
    }
    if (!runtime.services().register_factory(
            "probe", [state] { return std::make_unique<ProbeService>(state); })) {
        return fail("failed to register probe service");
    }
    if (auto started = runtime.start(); !started) {
        return fail(started.error().message());
    }

    auto launched = runtime.launch("probe", "primary");
    if (!launched || *launched != 0x0100'0001U) {
        runtime.request_stop();
        runtime.join();
        return fail("failed to launch primary service with compatible handle");
    }
    if (!runtime.name(*launched, ".probe") || runtime.resolve(".probe") != *launched) {
        runtime.request_stop();
        runtime.join();
        return fail("service naming or lookup failed");
    }

    const auto send_text = [&](std::string_view text, int session) {
        const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
        return runtime.send(
            *launched,
            0,
            static_cast<runtime::ProtocolType>(runtime::MessageType::lua),
            session,
            bytes);
    };
    if (!send_text("exercise-commands", 1)) {
        runtime.request_stop();
        runtime.join();
        return fail("failed to send command exercise message");
    }

    constexpr int kProducerCount = 4;
    constexpr int kMessagesPerProducer = 250;
    std::vector<std::jthread> producers;
    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer] {
            for (int index = 0; index < kMessagesPerProducer; ++index) {
                const std::string payload = "message-" + std::to_string(producer) +
                    "-" + std::to_string(index);
                (void)send_text(payload, 100 + producer * kMessagesPerProducer + index);
            }
        });
    }
    producers.clear();

    constexpr std::size_t kExpectedMessages =
        2 + kProducerCount * kMessagesPerProducer;
    if (!wait_for_messages(state, kExpectedMessages)) {
        runtime.request_stop();
        runtime.join();
        return fail("runtime did not dispatch all messages and timeout");
    }

    const bool timeout_seen = wait_for_message(
        state,
        [&](const auto& message) {
            return message.type == static_cast<runtime::ProtocolType>(
                       runtime::MessageType::response) &&
                message.session == state->timeout_session;
        });
    runtime::Message arbitrary{
        .source = 0,
        .session = 9000,
        .type = 255,
        .data = {},
    };
    if (!runtime.send(*launched, std::move(arbitrary))) {
        runtime.request_stop();
        runtime.join();
        return fail("arbitrary protocol type was not dispatched");
    }
    const bool protocol_255_seen = wait_for_message(
        state,
        [](const auto& message) {
            return message.type == 255;
        });

    bool producer_order_ok = true;
    {
        std::lock_guard lock(state->mutex);
        std::vector<int> next(kProducerCount, 0);
        for (const auto& message : state->messages) {
            const auto payload = message.string_payload();
            if (!payload.starts_with("message-")) { continue; }
            const int id = message.session - 100;
            const int producer = id / kMessagesPerProducer;
            const int index = id % kMessagesPerProducer;
            if (producer < 0 || producer >= kProducerCount || next[producer]++ != index ||
                payload != "message-" + std::to_string(producer) + "-" + std::to_string(index)) {
                producer_order_ok = false;
                break;
            }
        }
        producer_order_ok = producer_order_ok && std::ranges::all_of(next, [](int count) {
            return count == kMessagesPerProducer;
        });
    }

    if (!timeout_seen || !protocol_255_seen || !producer_order_ok || !state->commands_ok ||
        state->signal_count.load() != 1 ||
        state->maximum_dispatches.load() != 1) {
        std::cerr << "timeout_seen=" << timeout_seen
                  << " protocol_255_seen=" << protocol_255_seen
                  << " commands_ok=" << state->commands_ok
                  << " signal_count=" << state->signal_count.load()
                  << " maximum_dispatches=" << state->maximum_dispatches.load()
                  << '\n';
        runtime.request_stop();
        runtime.join();
        return fail("runtime compatibility or single-dispatch invariant failed");
    }

    if (!send_text("exit", 9999) || !wait_for_service_count(runtime, 0)) {
        runtime.request_stop();
        runtime.join();
        return fail("service EXIT did not retire the context");
    }

    if (!runtime.services().register_factory(
            "exit_init", [] { return std::make_unique<ExitDuringInitService>(); })) {
        runtime.request_stop();
        runtime.join();
        return fail("failed to register exit-during-init service");
    }
    if (runtime.launch("exit_init") || runtime.service_count() != 0) {
        runtime.request_stop();
        runtime.join();
        return fail("service that exits during init must not remain registered");
    }

    auto blocking_state = std::make_shared<BlockingState>();
    auto observer_state = std::make_shared<ObserverState>();
    if (!runtime.services().register_factory(
            "blocking",
            [blocking_state] { return std::make_unique<BlockingService>(blocking_state); }) ||
        !runtime.services().register_factory(
            "observer",
            [observer_state] { return std::make_unique<ObserverService>(observer_state); })) {
        runtime.request_stop();
        runtime.join();
        return fail("failed to register drop-message test services");
    }
    auto observer = runtime.launch("observer");
    auto blocking = runtime.launch("blocking");
    if (!observer || !blocking ||
        !runtime.send(*blocking, runtime::Message::text(0, 0, "block"))) {
        runtime.request_stop();
        runtime.join();
        return fail("failed to launch drop-message test services");
    }
    {
        std::unique_lock lock(blocking_state->mutex);
        if (!blocking_state->ready.wait_for(lock, 5s, [&] {
                return blocking_state->started;
            })) {
            runtime.request_stop();
            runtime.join();
            return fail("blocking service did not enter dispatch");
        }
    }
    if (!runtime.send(*blocking, runtime::Message::text(*observer, 42, "pending")) ||
        !runtime.retire(*blocking)) {
        runtime.request_stop();
        runtime.join();
        return fail("failed to retire service with a pending message");
    }
    {
        std::lock_guard lock(blocking_state->mutex);
        blocking_state->release = true;
    }
    blocking_state->ready.notify_all();
    {
        std::unique_lock lock(observer_state->mutex);
        if (!observer_state->ready.wait_for(lock, 5s, [&] {
                return std::ranges::any_of(observer_state->messages, [](const auto& message) {
                    return message.type == static_cast<runtime::ProtocolType>(
                               runtime::MessageType::error) &&
                        message.session == 42;
                });
            })) {
            runtime.request_stop();
            runtime.join();
            return fail("retired service did not report dropped request");
        }
    }
    (void)runtime.retire(*observer);

    auto aborting = runtime.launch("probe", "abort-target");
    if (!aborting ||
        !runtime.send(*aborting, runtime::Message::text(0, 0, "abort")) ||
        !wait_for_service_count(runtime, 0)) {
        runtime.request_stop();
        runtime.join();
        return fail("ABORT command did not retire all services");
    }

    runtime.request_stop();
    runtime.join();
    if (runtime.running()) {
        return fail("runtime should be stopped after join");
    }

    auto delayed_factory = std::make_shared<BlockingState>();
    runtime::Runtime stopping_runtime;
    if (!stopping_runtime.services().register_factory(
            "delayed", [delayed_factory] {
                std::unique_lock lock(delayed_factory->mutex);
                delayed_factory->started = true;
                delayed_factory->ready.notify_all();
                delayed_factory->ready.wait(lock, [delayed_factory] {
                    return delayed_factory->release;
                });
                return std::make_unique<ProbeService>(
                    std::make_shared<ProbeState>());
            }) ||
        !stopping_runtime.start()) {
        return fail("failed to prepare launch-stop race test");
    }
    std::atomic<bool> launch_succeeded{false};
    std::jthread launcher([&] {
        launch_succeeded.store(
            stopping_runtime.launch("delayed").has_value(),
            std::memory_order_release);
    });
    bool factory_started = false;
    {
        std::unique_lock lock(delayed_factory->mutex);
        factory_started = delayed_factory->ready.wait_for(lock, 5s, [&] {
            return delayed_factory->started;
        });
    }
    stopping_runtime.request_stop();
    std::atomic<bool> joined{false};
    std::jthread joiner([&] {
        stopping_runtime.join();
        joined.store(true);
    });
    std::this_thread::sleep_for(5ms);
    const bool joined_early = joined.load();
    {
        std::lock_guard lock(delayed_factory->mutex);
        delayed_factory->release = true;
    }
    delayed_factory->ready.notify_all();
    launcher.join();
    joiner.join();
    if (!factory_started) {
        return fail("delayed factory did not start");
    }
    if (joined_early || launch_succeeded.load(std::memory_order_acquire) ||
        stopping_runtime.service_count() != 0) {
        return fail("service escaped registration while runtime was stopping");
    }
    return EXIT_SUCCESS;
}
