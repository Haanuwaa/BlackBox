#pragma once

#include "core/clock.hpp"
#include "telemetry/provider.hpp"
#include "telemetry/windows/io_counter_tracker.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace blackbox::telemetry::windows {

struct WindowsSystemTimes {
    std::uint64_t idle_ticks{};
    std::uint64_t kernel_ticks_including_idle{};
    std::uint64_t user_ticks{};
};

struct WindowsPhysicalMemory {
    std::uint64_t total_bytes{};
    std::uint64_t available_bytes{};
};

struct WindowsTelemetryFunctions {
    bool (*read_system_times)(WindowsSystemTimes&) noexcept = nullptr;
    bool (*read_physical_memory)(WindowsPhysicalMemory&) noexcept = nullptr;
    void* io_context{};
    MetricStatus (*read_disk_counters)(
        void*, IoEntityCounters*, std::size_t, std::size_t&) noexcept = nullptr;
    MetricStatus (*read_network_counters)(
        void*, IoEntityCounters*, std::size_t, std::size_t&) noexcept = nullptr;
    MetricStatus (*read_disk_quality)(void*, RawDiskQuality&) noexcept = nullptr;
    MetricStatus (*read_network_quality)(void*, RawNetworkQuality&) noexcept = nullptr;
};

[[nodiscard]] WindowsTelemetryFunctions default_windows_telemetry_functions() noexcept;

class WindowsTelemetryProvider final : public ITelemetryProvider {
public:
    explicit WindowsTelemetryProvider(const core::IMonotonicClock& clock) noexcept;
    WindowsTelemetryProvider(const core::IMonotonicClock& clock,
                             WindowsTelemetryFunctions functions) noexcept;
    ~WindowsTelemetryProvider() override;

    [[nodiscard]] ProviderSampleResult sample(
        SamplingRequest request,
        RawTelemetrySnapshot& destination) override;

    [[nodiscard]] PlatformCapabilities capabilities() const noexcept override;

private:
    static constexpr std::size_t maximum_io_entities = 128U;
    struct NativeState;

    const core::IMonotonicClock& clock_;
    WindowsTelemetryFunctions functions_{};
    std::unique_ptr<NativeState> native_state_{};
    std::array<IoEntityCounters, maximum_io_entities> io_buffer_{};
    IoCounterTracker<maximum_io_entities> disk_tracker_{};
    IoCounterTracker<maximum_io_entities> network_tracker_{};
    std::uint64_t sequence_{};
};

} // namespace blackbox::telemetry::windows
