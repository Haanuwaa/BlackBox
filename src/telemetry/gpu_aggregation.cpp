#include "telemetry/gpu_aggregation.hpp"

#include <algorithm>
#include <limits>

namespace blackbox::telemetry {
namespace {

[[nodiscard]] constexpr MetricStatus
unavailable_status(const MetricStatus current,
                   const MetricStatus candidate) noexcept {
  if (current == MetricStatus::inaccessible ||
      candidate == MetricStatus::inaccessible) {
    return MetricStatus::inaccessible;
  }
  if (current == MetricStatus::temporarily_unavailable ||
      candidate == MetricStatus::temporarily_unavailable) {
    return MetricStatus::temporarily_unavailable;
  }
  return MetricStatus::unsupported;
}

} // namespace

GpuAggregateReading aggregate_gpu_devices(
    const std::span<const GpuDeviceReading> devices) noexcept {
  GpuAggregateReading result{};
  auto usage_status = MetricStatus::unsupported;
  auto memory_status = MetricStatus::unsupported;
  double maximum_usage{};
  std::uint64_t memory_total{};
  bool has_usage{};
  bool has_memory{};

  for (std::size_t index = 0U; index < devices.size(); ++index) {
    const auto &device = devices[index];
    if (device.private_identity == 0U) {
      usage_status = unavailable_status(usage_status,
                                        MetricStatus::temporarily_unavailable);
      memory_status = unavailable_status(memory_status,
                                         MetricStatus::temporarily_unavailable);
      continue;
    }
    const auto duplicate = std::find_if(
        devices.begin(), devices.begin() + static_cast<std::ptrdiff_t>(index),
        [identity = device.private_identity](const GpuDeviceReading &other) {
          return other.private_identity == identity;
        });
    if (duplicate != devices.begin() + static_cast<std::ptrdiff_t>(index)) {
      usage_status = unavailable_status(usage_status,
                                        MetricStatus::temporarily_unavailable);
      memory_status = unavailable_status(memory_status,
                                         MetricStatus::temporarily_unavailable);
      continue;
    }

    if (device.busiest_engine_usage.has_value()) {
      has_usage = true;
      maximum_usage =
          (std::max)(maximum_usage, device.busiest_engine_usage.value.value);
    } else {
      usage_status =
          unavailable_status(usage_status, device.busiest_engine_usage.status);
    }

    if (device.dedicated_memory_used.has_value()) {
      const auto value = device.dedicated_memory_used.value.value;
      if (value > (std::numeric_limits<std::uint64_t>::max)() - memory_total) {
        has_memory = false;
        memory_status = MetricStatus::temporarily_unavailable;
      } else if (memory_status != MetricStatus::temporarily_unavailable) {
        has_memory = true;
        memory_total += value;
      }
    } else {
      memory_status = unavailable_status(memory_status,
                                         device.dedicated_memory_used.status);
    }
  }

  result.busiest_engine_usage =
      has_usage ? MetricValue<Ratio>::available(Ratio{maximum_usage})
                : MetricValue<Ratio>::unavailable(usage_status);
  result.dedicated_memory_used =
      has_memory ? MetricValue<ByteCount>::available(ByteCount{memory_total})
                 : MetricValue<ByteCount>::unavailable(memory_status);
  return result;
}

} // namespace blackbox::telemetry
