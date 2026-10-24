#pragma once

#include <cstdint>
#include <ctime>

namespace skynet::runtime {
inline std::uint64_t thread_microseconds() noexcept {
    timespec time{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
    return static_cast<std::uint64_t>(time.tv_sec) * 1'000'000 + time.tv_nsec / 1000;
}
}  // namespace skynet::runtime
