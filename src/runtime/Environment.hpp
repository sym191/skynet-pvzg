#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace skynet::runtime {

class Environment {
 public:
  [[nodiscard]] bool set(std::string key, std::string value);
  [[nodiscard]] std::optional<std::string> get(std::string_view key) const;

 private:
  mutable std::mutex mutex_{};
  std::unordered_map<std::string, std::string> values_{};
};

}  // namespace skynet::runtime
