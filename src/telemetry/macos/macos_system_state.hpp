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

// Measures excess wake-up delay for a low-frequency utility timer. This is an
// observer of scheduler responsiveness, not a whole-system stall percentage.
class MacosSchedulerLatencyMonitor final {
public:
    MacosSchedulerLatencyMonitor() noexcept;
    ~MacosSchedulerLatencyMonitor();

    MacosSchedulerLatencyMonitor(const MacosSchedulerLatencyMonitor&) = delete;
    MacosSchedulerLatencyMonitor& operator=(const MacosSchedulerLatencyMonitor&) = delete;

    [[nodiscard]] MetricValue<Seconds> state() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_{};
    void* source_{};
};

} // namespace blackbox::telemetry::macos
