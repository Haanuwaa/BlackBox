#pragma once

#include "telemetry/types.hpp"

#include <cstdint>

namespace blackbox::telemetry::windows {

// GetSystemTimes reports kernel ticks including idle ticks. Convert that native
// shape into the portable cumulative busy/total contract.
[[nodiscard]] MetricValue<CpuTimeCounters> convert_system_times(
    std::uint64_t idle_ticks,
    std::uint64_t kernel_ticks_including_idle,
    std::uint64_t user_ticks) noexcept;

} // namespace blackbox::telemetry::windows
