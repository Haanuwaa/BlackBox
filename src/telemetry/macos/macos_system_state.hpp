#pragma once

#include "telemetry/provider.hpp"

#include <memory>

namespace blackbox::telemetry::macos {

[[nodiscard]] MetricValue<bool> macos_low_power_mode() noexcept;
[[nodiscard]] MetricValue<ThermalPressureState> macos_thermal_pressure_state() noexcept;
[[nodiscard]] MetricValue<ProcessId> macos_frontmost_process_id() noexcept;
[[nodiscard]] GpuInventoryEvidence macos_gpu_inventory() noexcept;

class MacosMemoryPressureMonitor final {
public:
    MacosMemoryPressureMonitor() noexcept;
    ~MacosMemoryPressureMonitor();

    MacosMemoryPressureMonitor(const MacosMemoryPressureMonitor&) = delete;
    MacosMemoryPressureMonitor& operator=(const MacosMemoryPressureMonitor&) = delete;

    [[nodiscard]] MetricValue<MemoryPressureState> state() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_{};
    void* source_{};
};

} // namespace blackbox::telemetry::macos
