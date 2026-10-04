#include "ServiceRegistry.hpp"

#include <mutex>
#include <utility>

namespace skynet::runtime {

bool ServiceRegistry::register_factory(std::string name, ServiceFactory factory) {
    if (name.empty() || !factory) {
        return false;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    auto [_, inserted] = factories_.emplace(std::move(name), std::move(factory));
    return inserted;
}

ServicePtr ServiceRegistry::create(std::string_view name) const {
    ServiceFactory factory;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = factories_.find(std::string(name));
        if (it == factories_.end()) {
            return nullptr;
        }
        factory = it->second;
    }
    return factory();
}

bool ServiceRegistry::contains(std::string_view name) const {
    std::lock_guard<std::mutex> guard(mutex_);
    return factories_.contains(std::string(name));
}

}  // namespace skynet::runtime
