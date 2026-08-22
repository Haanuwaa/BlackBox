#include "telemetry/windows/windows_counter_conversion.hpp"

#include <limits>

namespace blackbox::telemetry::windows {

MetricValue<CpuTimeCounters> convert_system_times(
    const std::uint64_t idle_ticks,
    const std::uint64_t kernel_ticks_including_idle,
    const std::uint64_t user_ticks) noexcept {
    if (kernel_ticks_including_idle < idle_ticks ||
        user_ticks > std::numeric_limits<std::uint64_t>::max() - kernel_ticks_including_idle) {
        return MetricValue<CpuTimeCounters>::unavailable(
            MetricStatus::temporarily_unavailable);
    }

    const auto total_ticks = kernel_ticks_including_idle + user_ticks;
    return MetricValue<CpuTimeCounters>::available(
        CpuTimeCounters{total_ticks - idle_ticks, total_ticks});
}

} // namespace blackbox::telemetry::windows
