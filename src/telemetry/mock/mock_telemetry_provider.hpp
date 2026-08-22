#pragma once

#include "core/clock.hpp"
#include "telemetry/provider.hpp"

#include <cstdint>

namespace blackbox::telemetry::mock {

enum class Scenario : std::uint8_t {
    normal,
    cpu_spike,
    disk_spike,
    network_drop,
    process_spike,
};

class MockTelemetryProvider final : public ITelemetryProvider {
public:
    explicit MockTelemetryProvider(const core::IMonotonicClock& clock,
                                   Scenario scenario = Scenario::normal);

    [[nodiscard]] ProviderSampleResult sample(
        SamplingRequest request,
        RawTelemetrySnapshot& destination) override;

    [[nodiscard]] PlatformCapabilities capabilities() const noexcept override;

    void set_capabilities(PlatformCapabilities capabilities) noexcept;
    void reset(Scenario scenario = Scenario::normal) noexcept;

private:
    void advance_counters() noexcept;

    const core::IMonotonicClock& clock_;
    Scenario scenario_;
    PlatformCapabilities capabilities_{};
    std::uint64_t sequence_{};
    CpuTimeCounters cpu_{};
    ByteCount disk_read_{};
    ByteCount disk_write_{};
    ByteCount network_receive_{};
    ByteCount network_transmit_{};
    std::chrono::nanoseconds process_cpu_time_{};
    ByteCount process_disk_read_{};
    ByteCount process_disk_write_{};
};

} // namespace blackbox::telemetry::mock
