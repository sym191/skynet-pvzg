#pragma once

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

namespace skynet::runtime {

class Context;

struct RuntimeOptions {
    std::size_t worker_count{8};
    std::uint8_t harbor_id{1};
    bool profile{true};
};

class Runtime final {
public:
    explicit Runtime(RuntimeOptions options = {});
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] std::expected<void, std::error_code> Start();
    void RequestStop() noexcept;
    void Join() noexcept;
    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] ServiceRegistry& services() noexcept;
    [[nodiscard]] Environment& environment() noexcept;
    [[nodiscard]] const Environment& environment() const noexcept;

    [[nodiscard]] std::expected<Handle, std::error_code> Launch(
        std::string_view service_name,
        std::string_view arguments = {}
    );
    [[nodiscard]] bool Retire(Handle handle);
    void AbortAll();

    [[nodiscard]] bool Send(Handle destination, Message message);
    [[nodiscard]] bool Send(
        Handle destination,
        Handle source,
        ProtocolType type,
        int session,
        std::span<const std::byte> payload
    );
    [[nodiscard]] Handle Resolve(std::string_view address) const;
    [[nodiscard]] bool Name(Handle handle, std::string name);

    [[nodiscard]] std::optional<std::string> Command(
        Context& context,
        std::string_view command,
        std::optional<std::string_view> parameter
    );

    void ScheduleTimeout(std::uint32_t ticks, Handle destination, int session);
    [[nodiscard]] std::uint64_t NowTicks() const noexcept;
    [[nodiscard]] std::uint32_t StartTime() const noexcept;
    [[nodiscard]] bool profile_enabled() const noexcept;
    [[nodiscard]] std::size_t service_count() const;

    void Log(Handle source, std::string_view message) const;

private:
    enum class State : std::uint8_t {
        stopped,
        running,
        stopping,
    };

    void MonitorLoop(std::stop_token stop_token) noexcept;
    void ReportDroppedMessages(Handle retired, std::vector<Message> messages);

    RuntimeOptions options_;
    Environment environment_;
    ServiceRegistry services_;
    HandleManager handles_;
    Scheduler scheduler_;
    TimerService timer_;
    std::atomic<State> state_{State::stopped};
    std::atomic<Handle> monitor_handle_{0};
    mutable std::mutex lifecycle_mutex_;
    std::mutex monitor_mutex_;
    std::condition_variable_any monitor_ready_;
    std::jthread monitor_worker_;
};

}  // namespace skynet::runtime
