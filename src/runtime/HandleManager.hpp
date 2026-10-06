#pragma once

#include "Message.hpp"

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace skynet::runtime {

class Context;

class HandleManager {
public:
    explicit HandleManager(std::uint8_t harbor_id = 1);

    [[nodiscard]] Handle register_context(
        const std::shared_ptr<Context>& context);
    [[nodiscard]] std::shared_ptr<Context> grab(Handle handle) const;
    [[nodiscard]] bool contains(Handle handle) const;
    // Keep the table entry alive without lending a producer its last reference.
    // The visitor must not reenter HandleManager; it may enqueue and schedule.
    template <class Visitor>
    bool with_context(Handle handle, Visitor&& visitor) const {
        std::shared_lock lock(mutex_);
        const auto found = contexts_.find(handle);
        return found != contexts_.end() && visitor(found->second);
    }
    [[nodiscard]] bool retire(Handle handle);
    [[nodiscard]] std::vector<std::shared_ptr<Context>> retire_all();

    [[nodiscard]] bool name_handle(Handle handle, std::string name);
    [[nodiscard]] Handle find_name(std::string_view name) const;
    [[nodiscard]] std::vector<Handle> handles() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::uint8_t harbor_id() const;

private:
    friend struct RuntimeTestAccess;
    [[nodiscard]] Handle make_handle(Handle local_id) const;

    mutable std::shared_mutex mutex_;

    Handle next_local_id_ = 1;
    std::uint8_t harbor_id_ = 1;
    std::unordered_map<Handle, std::shared_ptr<Context>> contexts_;
    std::unordered_map<std::string, Handle> names_;
};

}  // namespace skynet::runtime
