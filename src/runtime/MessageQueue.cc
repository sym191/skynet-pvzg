#include "MessageQueue.hpp"
#include "Message.hpp"

#include <mutex>
#include <optional>
#include <vector>

namespace skynet::runtime{

MessageQueue::MessageQueue(Handle owner): owner_(owner){}


void MessageQueue::set_owner(Handle owner){
    std::lock_guard<std::mutex> guard{mutex_};
    owner_ = owner;
}

bool MessageQueue::push(Message msg){
    std::lock_guard<std::mutex> guard{mutex_};
    queue_.push_back(std::move(msg));
    if(!scheduled_){
        scheduled_ = true;
        return true; // Needs to be added to global queue
    }
    return false;
}

std::optional<Message> MessageQueue::pop(){
    std::lock_guard<std::mutex> guard{mutex_};
    if(queue_.empty()){
        scheduled_ = false;
        return std::nullopt;
    }
    Message msg = std::move(queue_.front());
    queue_.pop_front();
    return msg;
}

bool MessageQueue::reschedule_after_dispatch(){
    std::lock_guard<std::mutex> guard{mutex_};
    if(queue_.empty()){
        scheduled_ = false;
        return false; // no more message to consume
    }
    return true;
}

std::size_t MessageQueue::length() const{
    std::lock_guard<std::mutex> guard{mutex_};
    return queue_.size();
}

void MessageQueue::clear(){
    std::lock_guard<std::mutex> guard{mutex_};
    queue_.clear();
    scheduled_ = false;
}

std::vector<Message> MessageQueue::drain(){
    std::vector<Message> msgs;
    std::lock_guard<std::mutex> guard(mutex_);
    msgs.reserve(queue_.size());
    while(!queue_.empty()){
        msgs.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    scheduled_ = false;
    return msgs;
}

} // skynet::runtime