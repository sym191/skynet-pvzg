#pragma once

#include "Message.hpp"

#include <memory>

namespace skynet::runtime {

class Context;

class Service{

public:
    Service() = default;
    virtual ~Service() = default;

    Service(const Service&) = delete;
    Service& operator = (const Service&) = delete;

    Service(Service&&) = delete;
    Service& operator = (Service&&) = delete;

    virtual bool init(Context& context, std::string_view args);
    virtual void dispatch(Context& context, Message msg) = 0;
    virtual void signal(Context& context, int signal);

};

using ServicePtr = std::unique_ptr<Service>;

}