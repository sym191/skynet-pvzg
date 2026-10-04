#include "HandleManager.hpp"

#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace skynet::runtime {

HandleManager::HandleManager(std::uint8_t harbor_id) : harbor_id_(harbor_id) {}

Handle HandleManager::register_context(const std::shared_ptr<Context>& context) {
    if (context == nullptr) {
        return 0;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (Handle i = 0; i < kHandleMask; ++i) {
        Handle local = next_local_id_;
        if (local == 0 || local > kHandleMask) {
            local = 1;
        }
        next_local_id_ = local + 1;
        Handle handle = make_handle(local);
        if (contexts_.contains(handle)) {
            continue;
        }
        contexts_.emplace(handle, context);
        return handle;
    }
    return 0;
}

Handle HandleManager::make_handle(Handle local_id) const {
    return (static_cast<Handle>(harbor_id_) << kHandleRemoteShift) |
        (local_id & kHandleMask);
}

std::shared_ptr<Context> HandleManager::grab(Handle handle) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = contexts_.find(handle);
    if (it == contexts_.end()) {
        return nullptr;
    }
    return it->second;
}

bool HandleManager::retire(Handle handle) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const bool erased = contexts_.erase(handle) > 0;
    if (erased) {
        for (auto it = names_.begin(); it != names_.end();) {
            if (it->second == handle) {
                it = names_.erase(it);
            } else {
                ++it;
            }
        }
    }
    return erased;
}

std::vector<std::shared_ptr<Context>> HandleManager::retire_all() {
    std::vector<std::shared_ptr<Context>> contexts;
    std::unique_lock<std::shared_mutex> lock(mutex_);
    contexts.reserve(contexts_.size());
    for (auto& [handle, context] : contexts_) {
        (void)handle;
        contexts.push_back(std::move(context));
    }
    contexts_.clear();
    names_.clear();
    return contexts;
}

bool HandleManager::name_handle(Handle handle, std::string name) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (name.empty() || !contexts_.contains(handle) || names_.contains(name)) {
        return false;
    }
    names_.emplace(std::move(name), handle);
    return true;
}

Handle HandleManager::find_name(std::string_view name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = names_.find(std::string(name));
    if (it == names_.end()) {
        return 0;
    }
    return it->second;
}

std::vector<Handle> HandleManager::handles() const {
    std::vector<Handle> res;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    res.reserve(contexts_.size());
    for (const auto& [handle, context] : contexts_) {
        (void)context;
        res.push_back(handle);
    }
    return res;
}

std::size_t HandleManager::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return contexts_.size();
}

std::uint8_t HandleManager::harbor_id() const {
    return harbor_id_;
}

}  // namespace skynet::runtime
