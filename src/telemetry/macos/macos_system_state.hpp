#pragma once

#include "telemetry/types.hpp"

namespace blackbox::telemetry::macos {

[[nodiscard]] MetricValue<bool> macos_low_power_mode() noexcept;
[[nodiscard]] MetricValue<ProcessId> macos_frontmost_process_id() noexcept;

} // namespace blackbox::telemetry::macos
