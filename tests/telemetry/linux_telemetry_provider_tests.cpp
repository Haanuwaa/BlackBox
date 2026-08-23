#include "core/clock.hpp"
#include "telemetry/linux/linux_telemetry_provider.hpp"
#include "telemetry/provider.hpp"

#include <catch2/catch_test_macros.hpp>

namespace core = blackbox::core;
namespace telemetry = blackbox::telemetry;
namespace linux_telemetry = blackbox::telemetry::linux;

TEST_CASE("Linux provider samples native system and process evidence through the portable "
          "contract",
          "[telemetry][linux][native]") {
  core::SystemMonotonicClock clock{};
  linux_telemetry::LinuxTelemetryProvider provider{clock};
  telemetry::RawTelemetrySnapshot snapshot{};
  const telemetry::SamplingRequest request{telemetry::SamplingTierSet::all()};

  const auto result = provider.sample(request, snapshot);
  CHECK(result.status == telemetry::ProviderSampleStatus::complete);
  CHECK(snapshot.system.cpu_time.has_value());
  CHECK(snapshot.system.memory_total.has_value());
  CHECK(snapshot.system.memory_available.has_value());
  CHECK(snapshot.system.logical_processor_count.has_value());
  CHECK(snapshot.system.disk_read_bytes.has_value());
  CHECK(snapshot.system.disk_write_bytes.has_value());
  CHECK(snapshot.system.network_receive_bytes.has_value());
  CHECK(snapshot.system.network_transmit_bytes.has_value());
  CHECK_FALSE(snapshot.processes.empty());
  CHECK(snapshot.processes.size() == snapshot.process_metadata.size());
  CHECK(telemetry::validate_provider_snapshot_contract(provider.capabilities(),
                                                       request, snapshot) ==
        telemetry::ProviderContractViolation::none);

  telemetry::RawTelemetrySnapshot second{};
  const auto second_result = provider.sample(request, second);
  CHECK(second_result.status == telemetry::ProviderSampleStatus::complete);
  CHECK(second_result.sequence == result.sequence + 1U);
  CHECK(telemetry::validate_provider_snapshot_contract(provider.capabilities(),
                                                       request, second) ==
        telemetry::ProviderContractViolation::none);
}
