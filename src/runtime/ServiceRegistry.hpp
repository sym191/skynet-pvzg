#pragma once

#include "Service.hpp"
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
namespace skynet::runtime {

using ServiceFactory = std::function<ServicePtr()>;

class ServiceRegistry{

public:
    bool register_factory(std::string name, ServiceFactory factory);
    [[nodiscard]] ServicePtr create(std::string_view name) const;
    [[nodiscard]] bool contains(std::string_view) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ServiceFactory> factories_;

};

}