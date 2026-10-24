#include "MessageQueue.hpp"

#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace skynet::runtime {

namespace {

constexpr std::size_t kInitialOverloadThreshold = 1024;

}  // namespace

MessageQueue::MessageQueue(Handle owner) : owner_(owner) {}

Handle MessageQueue::owner() const {
    std::lock_guard<std::mutex> guard{mutex_};
    return owner_;
}

void MessageQueue::set_owner(Handle owner) {
    std::lock_guard<std::mutex> guard{mutex_};
    owner_ = owner;
}

bool MessageQueue::push(Message message) {
    std::lock_guard<std::mutex> guard{mutex_};
    queue_.push_back(std::move(message));
    if (queue_.size() > overload_threshold_) {
        overload_ = queue_.size();
        overload_threshold_ *= 2;
    }
    if (!scheduled_) {
        scheduled_ = true;
        return true;
    }
    return false;
}

std::optional<Message> MessageQueue::pop() {
    std::lock_guard<std::mutex> guard{mutex_};
    if (queue_.empty()) {
        scheduled_ = false;
        overload_ = 0;
        overload_threshold_ = kInitialOverloadThreshold;
        return std::nullopt;
    }
    Message message = std::move(queue_.front());
    queue_.pop_front();
    return message;
}

bool MessageQueue::reschedule_after_dispatch() {
    std::lock_guard<std::mutex> guard{mutex_};
    if (queue_.empty()) {
        scheduled_ = false;
        overload_ = 0;
        overload_threshold_ = kInitialOverloadThreshold;
        return false;
    }
    return true;
}

std::size_t MessageQueue::length() const {
    std::lock_guard<std::mutex> guard{mutex_};
    return queue_.size();
}

std::size_t MessageQueue::take_overload() {
    std::lock_guard<std::mutex> guard{mutex_};
    return std::exchange(overload_, 0);
}

void MessageQueue::clear() {
    std::lock_guard<std::mutex> guard{mutex_};
    queue_.clear();
    scheduled_ = false;
    overload_ = 0;
    overload_threshold_ = kInitialOverloadThreshold;
}

std::vector<Message> MessageQueue::drain() {
    std::vector<Message> messages;
    std::lock_guard<std::mutex> guard{mutex_};
    messages.reserve(queue_.size());
    while (!queue_.empty()) {
        messages.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    scheduled_ = false;
    overload_ = 0;
    overload_threshold_ = kInitialOverloadThreshold;
    return messages;
}

}  // namespace skynet::runtime
