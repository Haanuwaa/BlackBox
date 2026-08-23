#pragma once

#include "core/clock.hpp"
#include "telemetry/provider.hpp"

#include <cstdint>
#include <memory>

namespace blackbox::telemetry::macos {

class MacosTelemetryProvider final : public ITelemetryProvider {
public:
    explicit MacosTelemetryProvider(const core::IMonotonicClock& clock) noexcept;
    ~MacosTelemetryProvider() override;

    [[nodiscard]] ProviderSampleResult sample(
        SamplingRequest request,
        RawTelemetrySnapshot& destination) override;
    [[nodiscard]] PlatformCapabilities capabilities() const noexcept override;

private:
    struct NativeState;
    const core::IMonotonicClock& clock_;
    std::unique_ptr<NativeState> native_state_{};
    std::uint64_t sequence_{};
};

} // namespace blackbox::telemetry::macos
