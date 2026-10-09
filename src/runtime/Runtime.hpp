#pragma once

#include "skynet/network/runtime.hpp"

#include "Environment.hpp"
#include "HandleManager.hpp"
#include "Message.hpp"
#include "Scheduler.hpp"
#include "ServiceRegistry.hpp"
#include "TimerService.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace skynet::runtime {

class Context;

struct RuntimeOptions {
    std::size_t worker_count{8};
    std::uint8_t harbor_id{1};
    bool profile{true};
};

struct Monitor{
    std::atomic<int> version{0};
    std::uint32_t check_version{0};

    Handle source{0};
    Handle destination{0};

    void trigger(Handle source, Handle destination);
    bool check();

};

class Runtime final : private network::EventSink {
public:
    explicit Runtime(RuntimeOptions options = {});
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // start/join/wait/destruction are owner-thread operations. Producers may
    // launch/send/retire/request_stop concurrently; join waits for active launches.
    [[nodiscard]] std::expected<void, std::error_code> start();
    void request_stop() noexcept;
    void join() noexcept;
    // Owner-thread only. Waits for ABORT/all services, then drains the logger.
    void wait();
    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] network::Runtime& sockets() noexcept { return network_; }
    [[nodiscard]] ServiceRegistry& services() noexcept;
    [[nodiscard]] Environment& environment() noexcept;
    [[nodiscard]] const Environment& environment() const noexcept;

    [[nodiscard]] std::expected<Handle, std::error_code> launch(
        std::string_view service_name,
        std::string_view arguments = {},
        bool bootstrap = false
    );
    [[nodiscard]] bool retire(Handle handle);
    [[nodiscard]] bool reserve(Handle handle);
    void abort_all();
    void initialization_failed(Handle handle);
    bool failed() const noexcept { return failed_.load(); }

    [[nodiscard]] bool send(Handle destination, Message message);
    [[nodiscard]] bool send_global(std::string_view name, Message message);
    void register_global(std::string name, Handle handle);
    [[nodiscard]] bool send(
        Handle destination,
        Handle source,
        ProtocolType type,
        int session,
        std::span<const std::byte> payload
    );
    [[nodiscard]] Handle resolve(std::string_view address) const;
    [[nodiscard]] bool name(Handle handle, std::string name);

    [[nodiscard]] std::optional<std::string> command(
        Context& context,
        std::string_view command,
        std::optional<std::string_view> parameter
    );

    void schedule_timeout(std::uint32_t ticks, Handle destination, int session);
    [[nodiscard]] std::uint64_t now_ticks() const noexcept;
    [[nodiscard]] std::uint32_t start_time() const noexcept;
    [[nodiscard]] bool profile_enabled() const noexcept;
    [[nodiscard]] std::size_t service_count() const;

    void log(Handle source, std::string_view message);

private:
    struct LaunchGuard {
        Runtime& runtime;
        ~LaunchGuard();
    };
    enum class State : std::uint8_t {
        stopped,
        running,
        stopping,
    };

    bool publish(network::Event&& event) noexcept override;
    bool owner_alive(network::ServiceId owner) const noexcept override;
    void monitor_loop(std::stop_token stop_token) noexcept;
    std::size_t service_count_locked() const;
    void report_dropped_messages(Handle retired, std::vector<Message> messages);

    network::Runtime network_;
    RuntimeOptions options_;
    Environment environment_;
    ServiceRegistry services_;
    HandleManager handles_;
    Scheduler scheduler_;
    TimerService timer_;
    std::atomic<State> state_{State::stopped};
    Monitor monitor_;
    std::atomic<Handle> monitor_handle_{0};
    mutable std::mutex lifecycle_mutex_;
    std::mutex monitor_mutex_;
    std::condition_variable_any monitor_ready_;
    std::jthread monitor_worker_;
    std::condition_variable lifecycle_ready_;
    bool abort_requested_{false};
    bool abort_complete_{false};
    bool stop_in_progress_{false};
    std::size_t active_launches_{0};
    std::unordered_map<Handle, std::shared_ptr<Context>> reserved_;
    std::atomic<Handle> bootstrap_handle_{0};
    std::atomic<bool> failed_{false};
    std::mutex global_mutex_;
    std::unordered_map<std::string, Handle> global_names_;
    std::unordered_map<std::string, std::vector<Message>> pending_global_;
};

}  // namespace skynet::runtime
