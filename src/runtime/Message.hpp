#pragma once


#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace skynet::runtime {

using Handle = uint32_t;

constexpr Handle kHandleMask = 0x00ff'ffffU;
constexpr int kHandleRemoteShift = 24;

enum class MessageType : std::uint8_t {
    text = 0,
    response = 1,
    multicast = 2,
    client = 3,
    system = 4,
    harbor = 5,
    socket = 6,
    error = 7,
    queue = 8,
    debug = 9,
    lua = 10,
    snax = 11,
    trace = 12,
};

struct Message {
    Handle source = 0;
    int session = 0;
    MessageType type = MessageType::text;
    std::vector<std::byte> data;


    [[nodiscard]] static Message text(
            Handle source_handle,
            int session_id,
            std::string_view payload) {
        Message msg;
        msg.source = source_handle;
        msg.session = session_id;
        msg.type = MessageType::text;
        auto bytes = std::as_bytes(std::span(payload.data(), payload.size()));
        msg.data.resize(bytes.size());
        std::copy(bytes.begin(), bytes.end(), msg.data.begin());
        return msg;
    }
    [[nodiscard]] std::string text_payload() const {
        return std::string(
            reinterpret_cast<const char*>(data.data()),
            data.size()
        );
    }

};

}  // namespace skynet::runtime
