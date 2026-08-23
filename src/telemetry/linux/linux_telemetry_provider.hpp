#pragma once

#include "core/clock.hpp"
#include "telemetry/provider.hpp"

#include <cstdint>

namespace blackbox::telemetry::linux {

class LinuxTelemetryProvider final : public ITelemetryProvider {
public:
  explicit LinuxTelemetryProvider(const core::IMonotonicClock &clock) noexcept;

  [[nodiscard]] ProviderSampleResult
  sample(SamplingRequest request, RawTelemetrySnapshot &destination) override;
  [[nodiscard]] PlatformCapabilities capabilities() const noexcept override;

private:
  const core::IMonotonicClock &clock_;
  std::uint64_t sequence_{};
};

} // namespace blackbox::telemetry::linux
