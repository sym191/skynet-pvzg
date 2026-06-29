#include "Environment.hpp"
#include <mutex>
#include <optional>
#include <string>

namespace skynet::runtime {

bool Environment::set(std::string key, std::string value){
    std::lock_guard<std::mutex> guard{mutex_};
    auto [_, inserted] = values_.emplace(std::move(key), std::move(value));
    return inserted;
}

std::optional<std::string> Environment::get(std::string_view key) const{
    std::lock_guard<std::mutex> guard{mutex_};
    auto it = values_.find(std::string{key});
    if(it == values_.end()){
        return std::nullopt;
    }
    return it -> second;
}

}