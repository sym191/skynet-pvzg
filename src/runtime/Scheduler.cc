#include "Scheduler.hpp"

#include "Context.hpp"

#include <exception>
#include <utility>

namespace skynet::runtime {

Scheduler::~Scheduler() {
    RequestStop();
    Join();
}

bool Scheduler::Start(std::size_t worker_count) {
    if (worker_count == 0) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (running_) {
        return true;
    }

    ready_queue_.clear();
    try {
        workers_.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers_.emplace_back(
                [this](std::stop_token token) { WorkerLoop(token); });
        }
    } catch (...) {
        for (auto& worker : workers_) {
            worker.request_stop();
        }
        ready_queue_.wake_all();
        workers_.clear();
        return false;
    }
    running_ = true;
    return true;
}

void Scheduler::Schedule(std::shared_ptr<Context> context) {
    if (context != nullptr) {
        ready_queue_.push(std::move(context));
    }
}

void Scheduler::RequestStop() noexcept {
    std::lock_guard lock(mutex_);
    for (auto& worker : workers_) {
        worker.request_stop();
    }
    running_ = false;
    ready_queue_.wake_all();
}

void Scheduler::Join() noexcept {
    std::vector<std::jthread> workers;
    {
        std::lock_guard lock(mutex_);
        workers.swap(workers_);
        running_ = false;
    }
    workers.clear();
    ready_queue_.clear();
}

bool Scheduler::running() const noexcept {
    std::lock_guard lock(mutex_);
    return running_;
}

void Scheduler::WorkerLoop(std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
        auto context = ready_queue_.wait_pop(stop_token);
        if (context == nullptr) {
            continue;
        }
        context->DispatchOne();
        if (context->RescheduleAfterDispatch()) {
            ready_queue_.push(std::move(context));
        }
    }
}

}  // namespace skynet::runtime
