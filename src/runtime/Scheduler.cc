#include "Scheduler.hpp"

#include "Context.hpp"

#include <exception>
#include <utility>

namespace skynet::runtime {

Scheduler::~Scheduler() {
    request_stop();
    join();
}

bool Scheduler::start(std::size_t worker_count) {
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
                [this](std::stop_token token) { worker_loop(token); });
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

void Scheduler::schedule(std::shared_ptr<Context> context) {
    if (context != nullptr) {
        ready_queue_.push(std::move(context));
    }
}

void Scheduler::request_stop() noexcept {
    std::lock_guard lock(mutex_);
    for (auto& worker : workers_) {
        worker.request_stop();
    }
    running_ = false;
    ready_queue_.wake_all();
}

void Scheduler::join() noexcept {
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

void Scheduler::worker_loop(std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
        auto context = ready_queue_.wait_pop(stop_token);
        if (context == nullptr) {
            continue;
        }
        context->dispatch_one();
        if (context->reschedule_after_dispatch()) {
            ready_queue_.push(std::move(context));
        }
    }
}

}  // namespace skynet::runtime
