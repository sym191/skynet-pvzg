#pragma once

#include "OwnedBuffer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace skynet::runtime {

using Handle = std::uint32_t;
using ProtocolType = std::uint8_t;
using MessageBuffer = OwnedBuffer;

constexpr Handle kHandleMask = 0x00ff'ffffU;
constexpr int kHandleRemoteShift = 24;

enum class MessageType : ProtocolType {
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
    Handle source{0};
    int session{0};
    ProtocolType type{static_cast<ProtocolType>(MessageType::text)};
    MessageBuffer data;

    [[nodiscard]] static Message from_string(
        Handle source_handle,
        int session_id,
        ProtocolType protocol,
        std::string_view payload
    ) {
        Message message{
            .source = source_handle,
            .session = session_id,
            .type = protocol,
            .data = {},
        };
        const auto bytes = std::as_bytes(std::span(payload.data(), payload.size()));
        message.data.assign(bytes.begin(), bytes.end());
        return message;
    }

    [[nodiscard]] static Message text(
        Handle source_handle,
        int session_id,
        std::string_view payload
    ) {
        return from_string(
            source_handle,
            session_id,
            static_cast<ProtocolType>(MessageType::text),
            payload);
    }

    [[nodiscard]] std::string string_payload() const {
        if (data.empty()) { return {}; }
        return std::string(
            reinterpret_cast<const char*>(data.data()),
            data.size());
    }
};

}  // namespace skynet::runtime
