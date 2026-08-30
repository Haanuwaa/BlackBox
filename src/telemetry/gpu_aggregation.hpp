#pragma once

#include "telemetry/provider.hpp"

#include <cstdint>
#include <span>

namespace blackbox::telemetry {

// Provider-private identities are used only to reject duplicate observations.
// They must never be persisted, displayed, or exported.
struct GpuDeviceReading {
  std::uint64_t private_identity{};
  MetricValue<Ratio> busiest_engine_usage{};
  MetricValue<ByteCount> dedicated_memory_used{};
  friend constexpr bool operator==(const GpuDeviceReading &,
                                   const GpuDeviceReading &) = default;
};

struct GpuAggregateReading {
  MetricValue<Ratio> busiest_engine_usage{};
  MetricValue<ByteCount> dedicated_memory_used{};
  friend constexpr bool operator==(const GpuAggregateReading &,
                                   const GpuAggregateReading &) = default;
};

// Utilization is the maximum device value, matching the existing "busiest
// engine" semantics. Memory is a checked sum. A usable reading wins over an
// unavailable sibling; otherwise inaccessible outranks temporary, which
// outranks unsupported.
[[nodiscard]] GpuAggregateReading
aggregate_gpu_devices(std::span<const GpuDeviceReading> devices) noexcept;

} // namespace blackbox::telemetry
