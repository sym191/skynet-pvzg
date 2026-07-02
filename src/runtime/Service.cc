#include "Service.hpp"

namespace skynet::runtime {

bool Service::init(Context& context, std::string_view args) {
  (void)context;
  (void)args;
  return true;
}

void Service::signal(Context& context, int signal) {
  (void)context;
  (void)signal;
}

} // skynet::runtime