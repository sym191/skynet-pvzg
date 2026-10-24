#pragma once
#include "Context.hpp"
#include "Runtime.hpp"
#include "Service.hpp"

namespace skynet::service {
// Single-node harbor. Global name delivery is coordinated by Runtime.
class HarborService final : public runtime::Service {
public:
    bool init(runtime::Context& context, std::string_view arguments) override {
        return arguments.starts_with("0 ") && context.runtime().environment().get("harbor") == "0" &&
            context.runtime().reserve(context.handle());
    }
    void dispatch(runtime::Context& context, runtime::Message message) override {
        const auto text = message.string_payload();
        if (message.type == static_cast<runtime::ProtocolType>(runtime::MessageType::harbor) && text.starts_with("N ")) {
            context.runtime().register_global(text.substr(2), message.source);
        } else { context.log("unsupported single-node harbor message"); }
    }
};
}  // namespace skynet::service
