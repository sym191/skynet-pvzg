#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

namespace skynet::runtime {

class Context;

class GlobalQueue {
 public:
  void push(std::shared_ptr<Context> context);
  [[nodiscard]] std::shared_ptr<Context> try_pop();
  [[nodiscard]] std::shared_ptr<Context> wait_pop(std::stop_token stop_token);
  void clear();
  void wake_all();

 private:
  std::mutex mutex_;
  std::condition_variable_any ready_;
  std::deque<std::shared_ptr<Context>> queue_;
};

}  // namespace skynet::runtime
