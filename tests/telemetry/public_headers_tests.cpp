#include "core/circular_recorder.hpp"
#include "core/clock.hpp"
#include "core/incident.hpp"
#include "core/system_event.hpp"
#include "platform/global_hotkey.hpp"
#include "telemetry/collector.hpp"
#include "telemetry/event_collector.hpp"
#include "telemetry/event_provider.hpp"
#include "telemetry/incident_snapshot_builder.hpp"
#include "telemetry/mock/mock_telemetry_provider.hpp"
#include "telemetry/normalizer.hpp"
#include "telemetry/provider.hpp"
#include "telemetry/types.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("public telemetry headers are self-contained", "[telemetry][architecture]") {
    SUCCEED();
}

TEST_CASE("portable provider contract accepts a future backend and rejects capability lies",
          "[telemetry][architecture][provider-contract][cross-platform]") {
    using namespace blackbox::telemetry;
    const SamplingRequest request{SamplingTierSet::all()};
    RawTelemetrySnapshot snapshot{};
    snapshot.reset(blackbox::core::MonotonicTimePoint{}, request.tiers);
    snapshot.system.cpu_time = MetricValue<CpuTimeCounters>::available({40U, 100U});
    const PlatformCapabilities cpu_only{.cpu_usage = true};
    CHECK(validate_provider_snapshot_contract(cpu_only, request, snapshot) ==
          ProviderContractViolation::none);

    snapshot.system.network_receive_bytes = MetricValue<ByteCount>::available({1U});
    CHECK(validate_provider_snapshot_contract(cpu_only, request, snapshot) ==
          ProviderContractViolation::capability_status_mismatch);
    snapshot.system.network_receive_bytes = {};
    snapshot.system.cpu_time.value = {101U, 100U};
    CHECK(validate_provider_snapshot_contract(cpu_only, request, snapshot) ==
          ProviderContractViolation::invalid_cpu_counters);

    snapshot.system.cpu_time.value = {40U, 100U};
    snapshot.system.disk_quality.service_time =
        MetricValue<Seconds>::available({-0.001});
    const PlatformCapabilities disk_quality{
        .cpu_usage = true, .disk_service_time = true};
    CHECK(validate_provider_snapshot_contract(disk_quality, request, snapshot) ==
          ProviderContractViolation::invalid_disk_quality);
}
