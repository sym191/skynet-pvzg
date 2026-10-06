#pragma once

#include "OwnedBuffer.hpp"
#include "skynet/network/runtime.hpp"

namespace skynet::runtime {
[[nodiscard]] OwnedBuffer make_socket_message(network::Event&& event);
void handoff_socket_message(OwnedBuffer& buffer) noexcept;
}  // namespace skynet::runtime
