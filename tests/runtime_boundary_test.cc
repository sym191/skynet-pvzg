#include "Context.hpp"
#include "HandleManager.hpp"
#include "Runtime.hpp"
#include "TimerService.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace skynet::runtime {
// Test-only seeds avoid allocating millions of services or billions of sessions.
struct RuntimeTestAccess {
    static void seed(HandleManager& manager, Handle value) { manager.next_local_id_ = value; }
    static void seed(Context& context, int value) { context.next_session_.store(value); }
};
}  // namespace skynet::runtime

namespace {
namespace rt = skynet::runtime;
using namespace std::chrono_literals;
void check(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}
class Empty final : public rt::Service {
public:
    void dispatch(rt::Context&, rt::Message) override {}
};
}  // namespace

int main() {
    try {
        rt::Runtime runtime;
        rt::HandleManager handles(7);
        auto first = std::make_shared<rt::Context>(runtime, std::make_unique<Empty>());
        check(handles.register_context(first) == 0x07000001, "first handle");
        rt::RuntimeTestAccess::seed(handles, rt::kHandleMask);
        auto last = std::make_shared<rt::Context>(runtime, std::make_unique<Empty>());
        check(handles.register_context(last) == 0x07ffffff, "maximum handle");
        auto wrapped = std::make_shared<rt::Context>(runtime, std::make_unique<Empty>());
        check(handles.register_context(wrapped) == 0x07000002, "wrap did not skip live handle");
        check(handles.grab(first->handle()) == first, "wrap replaced live service");
        check(handles.name_handle(first->handle(), "first"), "name first");
        check(handles.retire(first->handle()) && handles.find_name("first") == 0, "retire alias");

        rt::RuntimeTestAccess::seed(*wrapped, std::numeric_limits<int>::max() - 1);
        check(wrapped->new_session() == std::numeric_limits<int>::max(), "maximum session");
        check(wrapped->new_session() == 1 && wrapped->new_session() == 2, "session wrap");
        std::vector<int> sessions(4000);
        std::vector<std::jthread> producers;
        for (int producer = 0; producer != 4; ++producer) {
            producers.emplace_back([&, producer] {
                for (int index = 0; index != 1000; ++index) {
                    sessions[producer * 1000 + index] = wrapped->new_session();
                }
            });
        }
        producers.clear();
        std::ranges::sort(sessions);
        check(sessions.front() == 3 && sessions.back() == 4002 &&
              std::ranges::adjacent_find(sessions) == sessions.end(), "concurrent session uniqueness");
        check(wrapped->command("TIMEOUT", "-1").has_value() &&
              wrapped->command("TIMEOUT", "0").has_value() &&
              !wrapped->command("TIMEOUT", "bad"), "TIMEOUT input boundaries");

        std::mutex mutex;
        std::condition_variable ready;
        std::vector<int> fired;
        auto callback = [&](rt::Handle destination, int session) {
            std::lock_guard lock(mutex);
            if (destination == 23) { fired.push_back(session); }
            ready.notify_all();
        };
        rt::TimerService timer; // Must join before the callback's captures are destroyed.
        check(timer.start(callback), "timer start");
        timer.schedule(0, 23, 0);
        timer.schedule(2, 23, 2);
        timer.schedule(1, 23, 1);
        timer.schedule(std::numeric_limits<std::uint32_t>::max(), 23, 99);
        {
            std::unique_lock lock(mutex);
            check(ready.wait_for(lock, 2s, [&] { return fired.size() == 3; }), "timer timeout");
            check(fired == std::vector<int>({0, 1, 2}), "timer ordering");
        }
        timer.request_stop();
        timer.join();
        timer.schedule(0, 23, 99);
        check(timer.start(callback), "timer restart");
        timer.schedule(0, 23, 3);
        timer.request_stop();
        timer.join();
        check(fired == std::vector<int>({0, 1, 2, 3}), "stop retained a pending timer");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
