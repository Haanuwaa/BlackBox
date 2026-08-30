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
  CHECK(snapshot.system.disk_quality.queue_depth.status !=
        telemetry::MetricStatus::unsupported);
  CHECK(snapshot.system.network_receive_bytes.has_value());
  CHECK(snapshot.system.network_transmit_bytes.has_value());
  CHECK(snapshot.system.network_quality.connectivity.has_value());
  CHECK(snapshot.system.network_quality.active_interfaces.has_value());
  CHECK(snapshot.system.network_quality.interface_change_counter.has_value());
  CHECK(snapshot.system.network_quality.tcp_out_segments.has_value());
  CHECK(snapshot.system.network_quality.tcp_retransmitted_segments.has_value());
  CHECK(snapshot.system.network_quality.tcp_failed_connections.has_value());
  CHECK(snapshot.system.network_quality.tcp_established_resets.has_value());
  CHECK(snapshot.system.power_source.has_value());
  CHECK(snapshot.system.system_uptime.has_value());
  CHECK(snapshot.system.system_uptime.value.value >= 0.0);
  CHECK_FALSE(snapshot.processes.empty());
  CHECK(snapshot.processes.size() == snapshot.process_metadata.size());
  CHECK(telemetry::validate_provider_snapshot_contract(provider.capabilities(),
                                                       request, snapshot) ==
        telemetry::ProviderContractViolation::none);

  telemetry::RawTelemetrySnapshot second{};
  const auto second_result = provider.sample(request, second);
  CHECK(second_result.status == telemetry::ProviderSampleStatus::complete);
  CHECK(second_result.sequence == result.sequence + 1U);
  CHECK(second.system.network_quality.interface_change_counter.has_value());
  CHECK(provider.capabilities().disk_latency);
  CHECK(provider.capabilities().disk_queue_depth);
  CHECK(provider.capabilities().disk_service_time);
  CHECK(provider.capabilities().cpu_frequency);
  CHECK_FALSE(provider.capabilities().gpu_usage);
  CHECK_FALSE(provider.capabilities().dpc_isr);
  CHECK(snapshot.system.foreground_gpu_usage.status ==
        telemetry::MetricStatus::unsupported);
  CHECK(second.system.cpu_current_mhz.status !=
        telemetry::MetricStatus::inaccessible);
#if defined(BLACKBOX_HAS_X11_FOREGROUND)
  CHECK(provider.capabilities().foreground_application);
#else
  CHECK_FALSE(provider.capabilities().foreground_application);
#endif
  CHECK(telemetry::validate_provider_snapshot_contract(provider.capabilities(),
                                                       request, second) ==
        telemetry::ProviderContractViolation::none);
}
