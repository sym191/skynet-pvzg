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

void UpdateMaximum(std::atomic<int>& maximum, int value) {
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
            const auto timeout = context.Command("TIMEOUT", "2");
            if (!timeout) {
                return false;
            }
            state_->timeout_session = std::stoi(*timeout);
        }
        return true;
    }

    void dispatch(runtime::Context& context, runtime::Message message) override {
        const int active = state_->active_dispatches.fetch_add(1) + 1;
        UpdateMaximum(state_->maximum_dispatches, active);
        std::this_thread::sleep_for(50us);

        const std::string payload = message.StringPayload();
        if (payload == "exercise-commands") {
            const auto reg = context.Command("REG");
            const auto query = context.Command("QUERY", ".probe");
            const auto marker = context.Command("GETENV", "marker");
            const auto set_dynamic = context.Command("SETENV", "dynamic value");
            const auto dynamic = context.Command("GETENV", "dynamic");
            const auto start_time = context.Command("STARTTIME");
            const auto stat_message = context.Command("STAT", "message");
            const auto named = context.Command("NAME", ".probe_alias " + *reg);
            const auto alias = context.Command("QUERY", ".probe_alias");
            const auto launched = context.Command("LAUNCH", "probe child");
            const auto monitor_set = context.Command("MONITOR", *reg);
            const auto monitor_get = context.Command("MONITOR");
            const auto signal = context.Command("SIGNAL", *reg + " 0x7");
            const auto log_on = context.Command("LOGON", *reg);
            const auto log_off = context.Command("LOGOFF", *reg);
            (void)set_dynamic;
            (void)monitor_set;
            (void)signal;
            (void)log_on;
            (void)log_off;
            if (launched) {
                (void)context.Command("KILL", *launched);
            }
            state_->commands_ok = reg && query && marker && dynamic && start_time &&
                stat_message && named && alias &&
                monitor_get && *reg == *query && *marker == "available" &&
                *dynamic == "value" && *named == ".probe_alias" &&
                *alias == *reg && *monitor_get == *reg &&
                std::stoul(*start_time) > 0 && std::stoull(*stat_message) > 0;
        } else if (payload == "exit") {
            (void)context.Command("EXIT");
        } else if (payload == "abort") {
            (void)context.Command("ABORT");
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
        (void)context.Command("EXIT");
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

int Fail(std::string_view message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

bool WaitForMessages(
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
bool WaitForMessage(
    const std::shared_ptr<ProbeState>& state,
    Predicate predicate,
    std::chrono::milliseconds timeout = 5s
) {
    std::unique_lock lock(state->mutex);
    return state->ready.wait_for(lock, timeout, [&] {
        return std::ranges::any_of(state->messages, predicate);
    });
}

bool WaitForServiceCount(
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
        return Fail("failed to initialize runtime environment");
    }
    if (!runtime.services().register_factory(
            "probe", [state] { return std::make_unique<ProbeService>(state); })) {
        return Fail("failed to register probe service");
    }
    if (auto started = runtime.Start(); !started) {
        return Fail(started.error().message());
    }

    auto launched = runtime.Launch("probe", "primary");
    if (!launched || *launched != 0x0100'0001U) {
        runtime.RequestStop();
        runtime.Join();
        return Fail("failed to launch primary service with compatible handle");
    }
    if (!runtime.Name(*launched, ".probe") || runtime.Resolve(".probe") != *launched) {
        runtime.RequestStop();
        runtime.Join();
        return Fail("service naming or lookup failed");
    }

    const auto send_text = [&](std::string_view text, int session) {
        const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
        return runtime.Send(
            *launched,
            0,
            static_cast<runtime::ProtocolType>(runtime::MessageType::lua),
            session,
            bytes);
    };
    if (!send_text("exercise-commands", 1)) {
        runtime.RequestStop();
        runtime.Join();
        return Fail("failed to send command exercise message");
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
    if (!WaitForMessages(state, kExpectedMessages)) {
        runtime.RequestStop();
        runtime.Join();
        return Fail("runtime did not dispatch all messages and timeout");
    }

    const bool timeout_seen = WaitForMessage(
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
    if (!runtime.Send(*launched, std::move(arbitrary))) {
        runtime.RequestStop();
        runtime.Join();
        return Fail("arbitrary protocol type was not dispatched");
    }
    const bool protocol_255_seen = WaitForMessage(
        state,
        [](const auto& message) {
            return message.type == 255;
        });

    if (!timeout_seen || !protocol_255_seen || !state->commands_ok ||
        state->signal_count.load() != 1 ||
        state->maximum_dispatches.load() != 1) {
        std::cerr << "timeout_seen=" << timeout_seen
                  << " protocol_255_seen=" << protocol_255_seen
                  << " commands_ok=" << state->commands_ok
                  << " signal_count=" << state->signal_count.load()
                  << " maximum_dispatches=" << state->maximum_dispatches.load()
                  << '\n';
        runtime.RequestStop();
        runtime.Join();
        return Fail("runtime compatibility or single-dispatch invariant failed");
    }

    if (!send_text("exit", 9999) || !WaitForServiceCount(runtime, 0)) {
        runtime.RequestStop();
        runtime.Join();
        return Fail("service EXIT did not retire the context");
    }

    if (!runtime.services().register_factory(
            "exit_init", [] { return std::make_unique<ExitDuringInitService>(); })) {
        runtime.RequestStop();
        runtime.Join();
        return Fail("failed to register exit-during-init service");
    }
    if (runtime.Launch("exit_init") || runtime.service_count() != 0) {
        runtime.RequestStop();
        runtime.Join();
        return Fail("service that exits during init must not remain registered");
    }

    auto blocking_state = std::make_shared<BlockingState>();
    auto observer_state = std::make_shared<ObserverState>();
    if (!runtime.services().register_factory(
            "blocking",
            [blocking_state] { return std::make_unique<BlockingService>(blocking_state); }) ||
        !runtime.services().register_factory(
            "observer",
            [observer_state] { return std::make_unique<ObserverService>(observer_state); })) {
        runtime.RequestStop();
        runtime.Join();
        return Fail("failed to register drop-message test services");
    }
    auto observer = runtime.Launch("observer");
    auto blocking = runtime.Launch("blocking");
    if (!observer || !blocking ||
        !runtime.Send(*blocking, runtime::Message::Text(0, 0, "block"))) {
        runtime.RequestStop();
        runtime.Join();
        return Fail("failed to launch drop-message test services");
    }
    {
        std::unique_lock lock(blocking_state->mutex);
        if (!blocking_state->ready.wait_for(lock, 5s, [&] {
                return blocking_state->started;
            })) {
            runtime.RequestStop();
            runtime.Join();
            return Fail("blocking service did not enter dispatch");
        }
    }
    if (!runtime.Send(*blocking, runtime::Message::Text(*observer, 42, "pending")) ||
        !runtime.Retire(*blocking)) {
        runtime.RequestStop();
        runtime.Join();
        return Fail("failed to retire service with a pending message");
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
            runtime.RequestStop();
            runtime.Join();
            return Fail("retired service did not report dropped request");
        }
    }
    (void)runtime.Retire(*observer);

    auto aborting = runtime.Launch("probe", "abort-target");
    if (!aborting ||
        !runtime.Send(*aborting, runtime::Message::Text(0, 0, "abort")) ||
        !WaitForServiceCount(runtime, 0)) {
        runtime.RequestStop();
        runtime.Join();
        return Fail("ABORT command did not retire all services");
    }

    runtime.RequestStop();
    runtime.Join();
    if (runtime.running()) {
        return Fail("runtime should be stopped after join");
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
        !stopping_runtime.Start()) {
        return Fail("failed to prepare launch-stop race test");
    }
    std::atomic<bool> launch_succeeded{false};
    std::jthread launcher([&] {
        launch_succeeded.store(
            stopping_runtime.Launch("delayed").has_value(),
            std::memory_order_release);
    });
    bool factory_started = false;
    {
        std::unique_lock lock(delayed_factory->mutex);
        factory_started = delayed_factory->ready.wait_for(lock, 5s, [&] {
            return delayed_factory->started;
        });
    }
    stopping_runtime.RequestStop();
    {
        std::lock_guard lock(delayed_factory->mutex);
        delayed_factory->release = true;
    }
    delayed_factory->ready.notify_all();
    launcher.join();
    stopping_runtime.Join();
    if (!factory_started) {
        return Fail("delayed factory did not start");
    }
    if (launch_succeeded.load(std::memory_order_acquire) ||
        stopping_runtime.service_count() != 0) {
        return Fail("service escaped registration while runtime was stopping");
    }
    return EXIT_SUCCESS;
}
