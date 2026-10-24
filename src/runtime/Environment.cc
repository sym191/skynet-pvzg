#include "Environment.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace skynet::runtime {

bool Environment::set(std::string key, std::string value) {
    std::lock_guard<std::mutex> guard{mutex_};
    auto [iterator, inserted] = values_.emplace(std::move(key), std::move(value));
    (void)iterator;
    return inserted;
}

std::optional<std::string> Environment::get(std::string_view key) const {
    std::lock_guard<std::mutex> guard{mutex_};
    const auto iterator = values_.find(std::string(key));
    if (iterator == values_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

bool Environment::contains(std::string_view key) const {
    std::lock_guard<std::mutex> guard{mutex_};
    return values_.contains(std::string(key));
}

std::unordered_map<std::string, std::string> Environment::snapshot() const {
    std::lock_guard<std::mutex> guard{mutex_};
    return values_;
}

}  // namespace skynet::runtime
