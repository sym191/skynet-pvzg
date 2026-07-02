#pragma once

#include "Message.hpp"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
namespace skynet::runtime {

class Context;

class HandleManager{

public:
    explicit HandleManager(std::uint8_t harbor_id = 1);
    
    [[nodiscard]]Handle register_context(const std::shared_ptr<Context>& context);
    [[nodiscard]] std::shared_ptr<Context> grab(Handle handle) const;
    [[nodiscard]] bool retire(Handle handle);

    [[nodiscard]] bool name_handle(Handle handle, std::string name);
    [[nodiscard]] Handle find_name(std::string_view) const;
    [[nodiscard]] std::vector<Handle> handles() const;
    [[nodiscard]] std::uint8_t harbor_id() const;

private:
    [[nodiscard]] Handle make_handle(Handle local_id) const;

    mutable std::mutex mutex_;

    Handle next_local_id_ = 1;
    uint8_t harbor_id_ = 1;
    std::unordered_map<Handle, std::shared_ptr<Context>> contexts_;
    std::unordered_map<std::string, Handle> names_;


};

}