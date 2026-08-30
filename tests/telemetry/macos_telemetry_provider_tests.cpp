#include "core/clock.hpp"
#include "telemetry/macos/macos_telemetry_provider.hpp"
#include "telemetry/provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <unistd.h>

namespace core = blackbox::core;
namespace macos = blackbox::telemetry::macos;
namespace telemetry = blackbox::telemetry;

TEST_CASE("macOS provider exposes native system and process evidence",
          "[telemetry][macos][integration]") {
    core::SystemMonotonicClock clock;
    macos::MacosTelemetryProvider provider{clock};
    telemetry::RawTelemetrySnapshot snapshot;

    const auto result = provider.sample({}, snapshot);
    CHECK(result.sequence == 1U);
    CHECK(result.status == telemetry::ProviderSampleStatus::complete);
    CHECK(snapshot.system.cpu_time.has_value());
    CHECK(snapshot.system.cpu_time.value.total_ticks >=
          snapshot.system.cpu_time.value.busy_ticks);
    CHECK(snapshot.system.logical_processor_count.has_value());
    CHECK(snapshot.system.logical_processor_count.value >= 1U);
    REQUIRE(snapshot.system.memory_total.has_value());
    REQUIRE(snapshot.system.memory_available.has_value());
    CHECK(snapshot.system.memory_total.value.value >=
          snapshot.system.memory_available.value.value);
    CHECK(snapshot.system.network_receive_bytes.has_value());
    CHECK(snapshot.system.network_transmit_bytes.has_value());
    CHECK(snapshot.system.disk_read_bytes.has_value());
    CHECK(snapshot.system.disk_write_bytes.has_value());
    CHECK(snapshot.system.disk_quality.queue_depth.status ==
          telemetry::MetricStatus::unsupported);
    CHECK(snapshot.system.network_quality.connectivity.has_value());
    CHECK(snapshot.system.network_quality.active_interfaces.has_value());
    CHECK(snapshot.system.network_quality.interface_change_counter.has_value());
    CHECK(snapshot.system.network_quality.tcp_out_segments.has_value());
    CHECK(snapshot.system.network_quality.tcp_retransmitted_segments.has_value());
    CHECK(snapshot.system.network_quality.tcp_failed_connections.has_value());
    CHECK(snapshot.system.network_quality.tcp_established_resets.status ==
          telemetry::MetricStatus::unsupported);
    CHECK(snapshot.system.power_source.has_value());
    CHECK(snapshot.system.battery_saver.has_value());
    CHECK(snapshot.system.system_uptime.has_value());
    CHECK(snapshot.system.system_uptime.value.value >= 0.0);
    CHECK(provider.capabilities().network_usage);
    CHECK(provider.capabilities().disk_throughput);
    CHECK(provider.capabilities().disk_latency);
    CHECK(provider.capabilities().disk_service_time);
    CHECK_FALSE(provider.capabilities().disk_queue_depth);
    CHECK(provider.capabilities().network_connectivity);
    CHECK(provider.capabilities().network_transport_quality);
    CHECK(provider.capabilities().gpu_inventory);
    CHECK(provider.capabilities().power_status);
    CHECK(provider.capabilities().foreground_application);
    CHECK_FALSE(provider.capabilities().gpu_usage);
    CHECK_FALSE(provider.capabilities().dpc_isr);
    CHECK(snapshot.system.foreground_process.status !=
          telemetry::MetricStatus::unsupported);
    CHECK(snapshot.system.foreground_gpu_usage.status ==
          telemetry::MetricStatus::unsupported);
    CHECK(provider.capabilities().system_uptime);
    CHECK_FALSE(snapshot.processes.empty());
    CHECK(telemetry::validate_provider_snapshot_contract(
              provider.capabilities(), {}, snapshot) ==
          telemetry::ProviderContractViolation::none);
}

TEST_CASE("macOS GPU inventory uses public non-identifying Metal evidence",
          "[telemetry][macos][gpu][privacy]") {
    core::SystemMonotonicClock clock;
    macos::MacosTelemetryProvider provider{clock};
    const auto inventory = provider.gpu_inventory();

    REQUIRE(inventory.device_count.has_value());
    REQUIRE(inventory.integrated_device_count.has_value());
    REQUIRE(inventory.discrete_device_count.has_value());
    REQUIRE(inventory.render_device_available.has_value());
    CHECK(inventory.device_count.value ==
          inventory.integrated_device_count.value +
              inventory.discrete_device_count.value);
    CHECK(inventory.render_device_available.value ==
          (inventory.device_count.value != 0U));
    CHECK(telemetry::validate_gpu_inventory_contract(
              provider.capabilities(), inventory) ==
          telemetry::ProviderContractViolation::none);
    CHECK_FALSE(provider.capabilities().gpu_usage);
    CHECK_FALSE(provider.capabilities().gpu_memory);
}

TEST_CASE("macOS process evidence identifies the current executable",
          "[telemetry][macos][process][integration]") {
    core::SystemMonotonicClock clock;
    macos::MacosTelemetryProvider provider{clock};
    telemetry::RawTelemetrySnapshot snapshot;
    REQUIRE(provider.sample({}, snapshot).status ==
            telemetry::ProviderSampleStatus::complete);

    const auto own_pid = static_cast<std::uint32_t>(::getpid());
    const auto process = std::find_if(
        snapshot.processes.begin(), snapshot.processes.end(),
        [own_pid](const telemetry::RawProcessCounters& value) {
            return value.identity.pid.value == own_pid;
        });
    REQUIRE(process != snapshot.processes.end());
    CHECK(process->identity.creation_token != 0U);
    CHECK(process->cpu_time.has_value());
    CHECK(process->working_set.has_value());
    CHECK(process->disk_read_bytes.has_value());
    CHECK(process->disk_write_bytes.has_value());

    const auto metadata = std::find_if(
        snapshot.process_metadata.begin(), snapshot.process_metadata.end(),
        [identity = process->identity](const telemetry::ProcessInfo& value) {
            return value.identity == identity;
        });
    REQUIRE(metadata != snapshot.process_metadata.end());
    CHECK(metadata->name.has_value());
    CHECK_FALSE(metadata->name.value.empty());
    CHECK(metadata->executable_path.has_value());
    CHECK_FALSE(metadata->executable_path.value.empty());
}
