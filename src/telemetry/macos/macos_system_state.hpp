#pragma once

#include "telemetry/provider.hpp"

namespace blackbox::telemetry::macos {

[[nodiscard]] MetricValue<bool> macos_low_power_mode() noexcept;
[[nodiscard]] MetricValue<ThermalPressureState> macos_thermal_pressure_state() noexcept;
[[nodiscard]] MetricValue<ProcessId> macos_frontmost_process_id() noexcept;
[[nodiscard]] GpuInventoryEvidence macos_gpu_inventory() noexcept;

} // namespace blackbox::telemetry::macos
