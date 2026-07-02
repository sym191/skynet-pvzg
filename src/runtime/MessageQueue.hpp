#pragma once

#include "Message.hpp"

#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace skynet::runtime{

class MessageQueue{
public:
    MessageQueue() = default;
    explicit MessageQueue(Handle owner);

    [[nodiscard]] Handle owner() const;
    void set_owner(Handle owner);

    [[nodiscard]] bool push(Message msg);
    [[nodiscard]] std::optional<Message> pop();
    [[nodiscard]] bool reschedule_after_dispatch();
    [[nodiscard]] std::size_t length() const;
    void clear();
    [[nodiscard]] std::vector<Message> drain();


private:
    Handle owner_{0};
    mutable std::mutex mutex_{};
    std::deque<Message> queue_{};
    bool scheduled_ = false;
};


} // skynet::runtime