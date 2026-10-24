#pragma once

#include "Service.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace skynet::runtime {

using ServiceFactory = std::function<ServicePtr()>;
using ServiceLoader = std::function<ServicePtr(std::string_view)>;

class ServiceRegistry {
public:
    bool register_factory(std::string name, ServiceFactory factory);
    [[nodiscard]] ServicePtr create(std::string_view name) const;
    [[nodiscard]] bool contains(std::string_view name) const;
    void set_loader(ServiceLoader loader);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ServiceFactory> factories_;
    ServiceLoader loader_;
};

}  // namespace skynet::runtime
