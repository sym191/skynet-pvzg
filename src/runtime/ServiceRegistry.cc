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
    ServiceLoader loader;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = factories_.find(std::string(name));
        if (it == factories_.end()) {
            loader = loader_;
        } else {
            factory = it->second;
        }
    }
    return factory ? factory() : loader ? loader(name) : nullptr;
}

void ServiceRegistry::set_loader(ServiceLoader loader) {
    std::lock_guard lock(mutex_);
    loader_ = std::move(loader);
}

bool ServiceRegistry::contains(std::string_view name) const {
    std::lock_guard<std::mutex> guard(mutex_);
    return factories_.contains(std::string(name));
}

}  // namespace skynet::runtime
