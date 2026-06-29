#include "GlobalQueue.hpp"
#include <mutex>

namespace skynet::runtime {

void GlobalQueue::push(std::shared_ptr<Context> context) {
    {    
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(context);
    }
    ready_.notify_one();
}

std::shared_ptr<Context> GlobalQueue::try_pop() {
    std::shared_ptr<Context> res;
    std::lock_guard<std::mutex> lock(mutex_);
    if(!queue_.empty()){
        res = std::move(queue_.front());
        queue_.pop_front();
    }
    return res;
}

std::shared_ptr<Context> GlobalQueue::wait_pop(std::stop_token stop_token) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, stop_token, [this] {return !queue_.empty();});
    if(queue_.empty()){
        return {};
    }
    auto context = std::move(queue_.front());
    queue_.pop_front();
    return context;
}
    
void GlobalQueue::clear() {
    std::deque<std::shared_ptr<Context>> contexts;
    {
        std::lock_guard lock{mutex_};
        contexts.swap(queue_);
    }
}

void GlobalQueue::wake_all() {
  ready_.notify_all();
}

}