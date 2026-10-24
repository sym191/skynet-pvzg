// C service ABI compatible with Skynet's gate. The actor owns all state.
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

extern "C" {
#include "skynet.h"
#include "skynet_socket.h"
}

namespace {
constexpr std::uint32_t kFrameLimit = 0x1000000;

std::uint32_t handle(std::string_view text) {
    if (!text.starts_with(':')) { return 0; }
    std::uint32_t result{};
    auto parsed = std::from_chars(text.data() + 1, text.data() + text.size(), result, 16);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? result : 0;
}

struct Connection {
    std::uint32_t agent{}, client{};
    std::string input;
    bool closing{};
};

class Gate final {
public:
    ~Gate() {
        if (!context_) { return; }
        for (auto& [id, unused] : connections_) { skynet_socket_close(context_, id); }
        if (listener_ >= 0) { skynet_socket_close(context_, listener_); }
    }

    int init(skynet_context* context, const char* arguments) {
        context_ = context;
        if (!arguments) { return 1; }
        std::istringstream input(arguments);
        char header;
        std::string watchdog, binding;
        if (!(input >> header >> watchdog >> binding >> client_tag_ >> max_connections_) ||
            (header != 'S' && header != 'L') || max_connections_ <= 0 ||
            client_tag_ < 0 || client_tag_ > 255) { return 1; }
        header_size_ = header == 'S' ? 2 : 4;
        if (!client_tag_) { client_tag_ = PTYPE_CLIENT; }
        if (!watchdog.starts_with('!')) {
            watchdog_ = skynet_queryname(context, watchdog.c_str());
            if (!watchdog_) { return 1; }
        }
        const auto separator = binding.rfind(':');
        const auto port_text = std::string_view(binding).substr(
            separator == std::string::npos ? 0 : separator + 1);
        int port;
        auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() ||
            port <= 0 || port > 65535) { return 1; }
        const auto host = separator == std::string::npos ? "" : binding.substr(0, separator);
        listener_ = skynet_socket_listen(context, host.c_str(), port, 128);
        if (listener_ < 0) { return 1; }
        skynet_callback(context, this, callback);
        skynet_socket_start(context, listener_);
        return 0;
    }

private:
    static int callback(skynet_context* context, void* user, int type, int,
                        std::uint32_t, const void* message, std::size_t size) {
        auto& self = *static_cast<Gate*>(user);
        try {
            if (type == PTYPE_SOCKET && size >= sizeof(skynet_socket_message)) {
                self.socket_message(*static_cast<const skynet_socket_message*>(message), size);
            } else if (type == PTYPE_TEXT) {
                self.command(std::string(static_cast<const char*>(message), size));
            } else if (type == PTYPE_CLIENT && size > 4) {
                auto* bytes = static_cast<const unsigned char*>(message);
                std::uint32_t id{};
                for (int i = 0; i < 4; ++i) { id |= std::uint32_t(bytes[size - 4 + i]) << (8 * i); }
                if (self.connections_.contains(id)) {
                    socket_sendbuffer buffer{static_cast<int>(id), SOCKET_BUFFER_RAWPOINTER, message, size - 4};
                    skynet_socket_sendbuffer(context, &buffer);
                }
            }
        } catch (...) {
            skynet_error(context, "gate dispatch failed");
            // Retire rather than keep a partially parsed connection alive.
            skynet_command(context, "EXIT", nullptr);
        }
        return 0; // Outer message remains owned by the actor.
    }

    void report(std::string text, int session = 0) {
        if (watchdog_) {
            skynet_send(context_, 0, watchdog_, PTYPE_TEXT, session, text.data(), text.size());
        }
    }
    void command(const std::string& message) {
        std::istringstream input(message);
        std::string command;
        int id;
        input >> command;
        if (command == "close") {
            if (listener_ >= 0) { skynet_socket_close(context_, std::exchange(listener_, -1)); }
        } else if (command == "broker") {
            std::string name;
            if (input >> name) { broker_ = skynet_queryname(context_, name.c_str()); }
        } else if (input >> id) {
            const auto found = connections_.find(id);
            if (found == connections_.end()) { return; }
            if (command == "kick") {
                found->second.closing = true;
                skynet_socket_close(context_, id);
            } else if (command == "start") {
                skynet_socket_start(context_, id);
            } else if (command == "forward") {
                std::string agent, client;
                if (input >> agent >> client) {
                    found->second.agent = handle(agent);
                    found->second.client = handle(client);
                }
            }
        }
    }
    void forward(int id, Connection& connection, std::string_view payload) {
        if (broker_ || connection.agent) {
            skynet_send(context_, broker_ ? 0 : connection.client,
                broker_ ? broker_ : connection.agent, client_tag_, id,
                const_cast<char*>(payload.data()), payload.size());
        } else {
            report(std::to_string(id) + " data " + std::string(payload), id);
        }
    }
    void data(int id, std::string_view bytes) {
        auto found = connections_.find(id);
        if (found == connections_.end()) { skynet_socket_close(context_, id); return; }
        auto& connection = found->second;
        if (connection.closing) { return; }
        connection.input.append(bytes);
        std::size_t offset = 0;
        while (connection.input.size() - offset >= header_size_) {
            std::uint32_t length{};
            for (std::size_t i = 0; i < header_size_; ++i) {
                length = (length << 8) | static_cast<unsigned char>(connection.input[offset + i]);
            }
            // Reject from the header alone, including lengths with bit 31 set.
            if (length >= kFrameLimit) {
                connection.input.clear();
                connection.closing = true;
                skynet_socket_close(context_, id);
                skynet_error(context_, "Recv socket message >= 16M");
                return;
            }
            if (connection.input.size() - offset - header_size_ < length) { break; }
            if (length) { // Upstream gate consumes empty frames without forwarding.
                forward(id, connection, std::string_view(connection.input).substr(offset + header_size_, length));
            }
            offset += header_size_ + length;
        }
        connection.input.erase(0, offset);
    }
    void socket_message(const skynet_socket_message& message, std::size_t size) {
        // Socket dispatch hands off nested malloc storage even if the fd is unknown.
        std::unique_ptr<char, decltype(&std::free)> payload(message.buffer, std::free);
        switch (message.type) {
        case SKYNET_SOCKET_TYPE_DATA:
            if (message.ud >= 0 && (message.buffer || !message.ud)) {
                data(message.id, std::string_view(message.buffer ? message.buffer : "", message.ud));
            }
            break;
        case SKYNET_SOCKET_TYPE_ACCEPT:
            if (message.id != listener_ || connections_.size() >= static_cast<std::size_t>(max_connections_)) {
                skynet_socket_close(context_, message.ud);
            } else {
                connections_.try_emplace(message.ud);
                const auto remote = std::string(reinterpret_cast<const char*>(&message + 1),
                    size - sizeof(message));
                report(std::to_string(message.ud) + " open " + std::to_string(message.ud) + " " + remote + ":0");
            }
            break;
        case SKYNET_SOCKET_TYPE_CLOSE:
        case SKYNET_SOCKET_TYPE_ERROR:
            if (connections_.erase(message.id)) {
                report(std::to_string(message.id) + " close");
                skynet_socket_close(context_, message.id);
            }
            break;
        case SKYNET_SOCKET_TYPE_CONNECT:
            if (message.id != listener_ && !connections_.contains(message.id)) {
                skynet_socket_close(context_, message.id);
            }
            break;
        default: break;
        }
    }
    skynet_context* context_{};
    int listener_{-1}, client_tag_{}, max_connections_{};
    std::size_t header_size_{};
    std::uint32_t watchdog_{}, broker_{};
    std::unordered_map<int, Connection> connections_;
};
}  // namespace

extern "C" {
void* gate_create() { try { return new Gate; } catch (...) { return nullptr; } }
int gate_init(void* gate, skynet_context* context, const char* arguments) {
    try { return static_cast<Gate*>(gate)->init(context, arguments); } catch (...) { return 1; }
}
void gate_release(void* gate) { delete static_cast<Gate*>(gate); }
}
