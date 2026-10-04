#pragma once

#include "GlobalQueue.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace skynet::runtime {

class Context;

class Scheduler final {
public:
    Scheduler() = default;
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    [[nodiscard]] bool Start(std::size_t worker_count);
    void Schedule(std::shared_ptr<Context> context);
    void RequestStop() noexcept;
    void Join() noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    void WorkerLoop(std::stop_token stop_token) noexcept;

    GlobalQueue ready_queue_;
    mutable std::mutex mutex_;
    std::vector<std::jthread> workers_;
    bool running_{false};
};

}  // namespace skynet::runtime
