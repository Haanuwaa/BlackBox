#include "core/clock.hpp"
#include "telemetry/normalizer.hpp"
#include "telemetry/process_normalizer.hpp"
#include "telemetry/windows/windows_telemetry_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <thread>
#include <process.h>
#include <vector>

namespace core = blackbox::core;
namespace telemetry = blackbox::telemetry;
namespace windows = blackbox::telemetry::windows;
using namespace std::chrono_literals;

namespace {

bool successful_times(windows::WindowsSystemTimes& result) noexcept {
    result = windows::WindowsSystemTimes{100U, 400U, 200U};
    return true;
}

bool successful_memory(windows::WindowsPhysicalMemory& result) noexcept {
    result = windows::WindowsPhysicalMemory{1'000U, 400U};
    return true;
}

bool failed_times(windows::WindowsSystemTimes&) noexcept {
    return false;
}

bool failed_memory(windows::WindowsPhysicalMemory&) noexcept {
    return false;
}

struct IoFixture {
    std::array<windows::IoEntityCounters, 2U> disk{
        windows::IoEntityCounters{1U, 100U, 200U},
        windows::IoEntityCounters{2U, 300U, 400U}};
    std::array<windows::IoEntityCounters, 1U> network{
        windows::IoEntityCounters{10U, 500U, 600U}};
};

telemetry::MetricStatus read_fixture_disk(
    void* context, windows::IoEntityCounters* destination,
    const std::size_t capacity, std::size_t& count) noexcept {
    const auto& fixture = *static_cast<IoFixture*>(context);
    if (capacity < fixture.disk.size()) {
        count = 0U;
        return telemetry::MetricStatus::temporarily_unavailable;
    }
    std::copy(fixture.disk.begin(), fixture.disk.end(), destination);
    count = fixture.disk.size();
    return telemetry::MetricStatus::available;
}

telemetry::MetricStatus read_fixture_network(
    void* context, windows::IoEntityCounters* destination,
    const std::size_t capacity, std::size_t& count) noexcept {
    const auto& fixture = *static_cast<IoFixture*>(context);
    if (capacity < fixture.network.size()) {
        count = 0U;
        return telemetry::MetricStatus::temporarily_unavailable;
    }
    std::copy(fixture.network.begin(), fixture.network.end(), destination);
    count = fixture.network.size();
    return telemetry::MetricStatus::available;
}

telemetry::MetricStatus failed_io(
    void*, windows::IoEntityCounters*, std::size_t, std::size_t& count) noexcept {
    count = 0U;
    return telemetry::MetricStatus::temporarily_unavailable;
}

telemetry::MetricStatus read_fixture_disk_quality(
    void*, telemetry::RawDiskQuality& destination) noexcept {
    destination.read_latency = telemetry::MetricValue<telemetry::Seconds>::available(
        telemetry::Seconds{0.002});
    destination.write_latency = telemetry::MetricValue<telemetry::Seconds>::available(
        telemetry::Seconds{0.003});
    destination.service_time = telemetry::MetricValue<telemetry::Seconds>::available(
        telemetry::Seconds{0.004});
    destination.queue_depth = telemetry::MetricValue<double>::available(1.0);
    destination.worst_device_id =
        telemetry::MetricValue<std::uint64_t>::available(1U);
    return telemetry::MetricStatus::available;
}

telemetry::MetricStatus read_fixture_network_quality(
    void*, telemetry::RawNetworkQuality& destination) noexcept {
    destination.connectivity =
        telemetry::MetricValue<telemetry::NetworkConnectivityLevel>::available(
            telemetry::NetworkConnectivityLevel::internet);
    destination.active_interfaces =
        telemetry::MetricValue<std::uint64_t>::available(1U);
    destination.interface_change_counter =
        telemetry::MetricValue<std::uint64_t>::available(0U);
    destination.tcp_out_segments =
        telemetry::MetricValue<std::uint64_t>::available(100U);
    destination.tcp_retransmitted_segments =
        telemetry::MetricValue<std::uint64_t>::available(0U);
    destination.tcp_failed_connections =
        telemetry::MetricValue<std::uint64_t>::available(0U);
    destination.tcp_established_resets =
        telemetry::MetricValue<std::uint64_t>::available(0U);
    return telemetry::MetricStatus::available;
}

telemetry::MetricStatus read_partial_network_quality(
    void*, telemetry::RawNetworkQuality& destination) noexcept {
    destination.connectivity =
        telemetry::MetricValue<telemetry::NetworkConnectivityLevel>::available(
            telemetry::NetworkConnectivityLevel::local);
    destination.active_interfaces =
        telemetry::MetricValue<std::uint64_t>::available(1U);
    destination.interface_change_counter =
        telemetry::MetricValue<std::uint64_t>::available(3U);
    destination.tcp_out_segments =
        telemetry::MetricValue<std::uint64_t>::unavailable(
            telemetry::MetricStatus::temporarily_unavailable);
    destination.tcp_retransmitted_segments = destination.tcp_out_segments;
    destination.tcp_failed_connections = destination.tcp_out_segments;
    destination.tcp_established_resets = destination.tcp_out_segments;
    return telemetry::MetricStatus::available;
}

windows::WindowsTelemetryFunctions fixture_functions(
    IoFixture& fixture,
    bool (*times)(windows::WindowsSystemTimes&) noexcept = successful_times,
    bool (*memory)(windows::WindowsPhysicalMemory&) noexcept = successful_memory) {
    return windows::WindowsTelemetryFunctions{
        times, memory, &fixture, read_fixture_disk, read_fixture_network,
        read_fixture_disk_quality, read_fixture_network_quality};
}

} // namespace

TEST_CASE("Windows provider reports implemented system capabilities",
          "[telemetry][windows][integration]") {
    core::SystemMonotonicClock clock;
    windows::WindowsTelemetryProvider provider{clock};
    const auto capabilities = provider.capabilities();

    CHECK(capabilities.cpu_usage);
    CHECK(capabilities.memory_usage);
    CHECK(capabilities.process_cpu);
    CHECK(capabilities.process_memory);
    CHECK(capabilities.process_disk_io);
    CHECK(capabilities.disk_throughput);
    CHECK(capabilities.network_usage);
    CHECK(capabilities.disk_latency);
    CHECK(capabilities.disk_queue_depth);
    CHECK(capabilities.disk_service_time);
    CHECK(capabilities.network_connectivity);
    CHECK(capabilities.network_transport_quality);
    CHECK(capabilities.gpu_inventory);
    const auto inventory = provider.gpu_inventory();
    CHECK(telemetry::validate_gpu_inventory_contract(capabilities, inventory) ==
          telemetry::ProviderContractViolation::none);
    if (inventory.device_count.has_value()) {
        CHECK(inventory.unknown_device_count.value == inventory.device_count.value);
        CHECK(inventory.integrated_device_count.value == 0U);
        CHECK(inventory.discrete_device_count.value == 0U);
    }
}

TEST_CASE("Windows provider samples internally consistent CPU and physical memory",
          "[telemetry][windows][integration]") {
    core::SystemMonotonicClock clock;
    windows::WindowsTelemetryProvider provider{clock};
    telemetry::SystemTelemetryNormalizer normalizer;
    telemetry::ProcessTelemetryNormalizer process_normalizer;
    std::vector<telemetry::ProcessSample> processes;
    auto raw_storage = std::make_unique<telemetry::RawTelemetrySnapshot>();
    auto& raw = *raw_storage;

    const auto first_result = provider.sample({}, raw);
    CHECK(first_result.status == telemetry::ProviderSampleStatus::complete);
    REQUIRE(raw.system.cpu_time.has_value());
    REQUIRE(raw.system.memory_total.has_value());
    REQUIRE(raw.system.memory_available.has_value());
    CHECK(raw.system.memory_total.value.value > 0U);
    CHECK(raw.system.memory_available.value <= raw.system.memory_total.value);
    REQUIRE(raw.system.disk_read_bytes.has_value());
    REQUIRE(raw.system.disk_write_bytes.has_value());
    CHECK_FALSE(raw.processes.empty());
    CHECK_FALSE(raw.process_metadata.empty());
    CHECK(raw.process_diagnostics.enumerated >= raw.processes.size());
    CHECK(raw.process_diagnostics.sampled == raw.processes.size());
    CHECK(telemetry::validate_provider_snapshot_contract(
              provider.capabilities(), {}, raw) ==
          telemetry::ProviderContractViolation::none);
    process_normalizer.normalize(raw, processes);
    CHECK(processes.size() == raw.processes.size());

    auto normalized = std::make_unique<telemetry::SystemSample>(normalizer.normalize(raw));
    CHECK(normalized->cpu_usage.status == telemetry::MetricStatus::temporarily_unavailable);
    REQUIRE(normalized->memory_usage.has_value());
    CHECK(normalized->disk_read_rate.status == telemetry::MetricStatus::temporarily_unavailable);
    CHECK(normalized->network_receive_rate.status ==
          telemetry::MetricStatus::temporarily_unavailable);
    CHECK(normalized->memory_usage.value.value >= 0.0);
    CHECK(normalized->memory_usage.value.value <= 1.0);

    std::this_thread::sleep_for(50ms);
    const auto second_result = provider.sample({}, raw);
    CHECK(second_result.status == telemetry::ProviderSampleStatus::complete);
    *normalized = normalizer.normalize(raw);
    process_normalizer.normalize(raw, processes);
    if (normalized->cpu_usage.has_value()) {
        CHECK(normalized->cpu_usage.value.value >= 0.0);
        CHECK(normalized->cpu_usage.value.value <= 1.0);
    } else {
        // A zero native tick delta over a very short interval is valid warming behavior.
        CHECK(normalized->cpu_usage.status == telemetry::MetricStatus::temporarily_unavailable);
    }
    REQUIRE(normalized->disk_read_rate.has_value());
    REQUIRE(normalized->disk_write_rate.has_value());
    REQUIRE(normalized->network_receive_rate.has_value());
    REQUIRE(normalized->network_transmit_rate.has_value());
    CHECK(normalized->disk_read_rate.value.value >= 0.0);
    CHECK(normalized->network_receive_rate.value.value >= 0.0);
    REQUIRE_FALSE(processes.empty());
    bool found_current_process = false;
    for (const auto& process : processes) {
        if (process.identity.pid.value == static_cast<std::uint32_t>(_getpid())) {
            found_current_process = true;
            REQUIRE(process.working_set.has_value());
            CHECK(process.working_set.value.value > 0U);
            if (process.cpu_usage.has_value()) {
                CHECK(process.cpu_usage.value.value >= 0.0);
                CHECK(process.cpu_usage.value.value <= 1.0);
            }
        }
    }
    CHECK(found_current_process);
}

TEST_CASE("Windows forensic gauges preserve availability and foreground privacy",
          "[telemetry][windows][gpu][responsiveness][power][privacy]") {
    core::SystemMonotonicClock clock;
    windows::WindowsTelemetryProvider provider{clock};
    telemetry::RawTelemetrySnapshot raw;
    telemetry::SamplingRequest request{};
    request.collect_foreground_application = false;

    REQUIRE(provider.sample(request, raw).status ==
            telemetry::ProviderSampleStatus::complete);
    CHECK(raw.system.foreground_process.status == telemetry::MetricStatus::unsupported);
    CHECK(raw.system.foreground_gpu_usage.status == telemetry::MetricStatus::unsupported);
    REQUIRE(raw.system.system_uptime.has_value());
    CHECK(raw.system.system_uptime.value.value > 0.0);
    if (raw.system.gpu_usage.has_value()) {
        CHECK(raw.system.gpu_usage.value.value >= 0.0);
        CHECK(raw.system.gpu_usage.value.value <= 1.0);
        REQUIRE(raw.system.gpu_dedicated_memory.has_value());
        REQUIRE(raw.system.gpu_shared_memory.has_value());
    } else {
        CHECK((raw.system.gpu_usage.status == telemetry::MetricStatus::unsupported ||
               raw.system.gpu_usage.status ==
                   telemetry::MetricStatus::temporarily_unavailable));
    }
    if (raw.system.dpc_usage.has_value()) {
        CHECK(raw.system.dpc_usage.value.value >= 0.0);
        CHECK(raw.system.dpc_usage.value.value <= 1.0);
        REQUIRE(raw.system.interrupt_usage.has_value());
        REQUIRE(raw.system.dpc_rate.has_value());
        CHECK(raw.system.dpc_rate.value >= 0.0);
    }
    if (raw.system.cpu_thermal_limit_fraction.has_value()) {
        CHECK(raw.system.cpu_thermal_limit_fraction.value.value >= 0.0);
        CHECK(raw.system.cpu_thermal_limit_fraction.value.value <= 1.0);
    }
    CHECK(telemetry::validate_provider_snapshot_contract(
              provider.capabilities(), request, raw) ==
          telemetry::ProviderContractViolation::none);
}

TEST_CASE("Windows provider respects fast and normal tier requests",
          "[telemetry][windows][integration]") {
    core::SystemMonotonicClock clock;
    windows::WindowsTelemetryProvider provider{clock};
    telemetry::RawTelemetrySnapshot raw;

    const telemetry::SamplingRequest fast_only{telemetry::SamplingTier::fast};
    CHECK(provider.sample(fast_only, raw).status ==
          telemetry::ProviderSampleStatus::complete);
    CHECK(raw.system.cpu_time.has_value());
    CHECK(raw.system.disk_read_bytes.has_value());
    CHECK(raw.system.network_receive_bytes.has_value());
    CHECK(raw.system.memory_total.status == telemetry::MetricStatus::temporarily_unavailable);

    const telemetry::SamplingRequest normal_only{telemetry::SamplingTier::normal};
    CHECK(provider.sample(normal_only, raw).status ==
          telemetry::ProviderSampleStatus::complete);
    CHECK(raw.system.cpu_time.status == telemetry::MetricStatus::temporarily_unavailable);
    CHECK(raw.system.memory_total.has_value());
}

TEST_CASE("Windows provider degrades partial and total API failures without stopping",
          "[telemetry][windows]") {
    core::SystemMonotonicClock clock;
    auto raw_storage = std::make_unique<telemetry::RawTelemetrySnapshot>();
    auto& raw = *raw_storage;

    IoFixture fixture;
    auto partial_provider = std::make_unique<windows::WindowsTelemetryProvider>(
        clock, fixture_functions(fixture, successful_times, failed_memory));
    CHECK(partial_provider->sample({}, raw).status ==
          telemetry::ProviderSampleStatus::partial);
    CHECK(raw.system.cpu_time.has_value());
    CHECK(raw.system.memory_total.status ==
          telemetry::MetricStatus::temporarily_unavailable);

    auto failed_provider = std::make_unique<windows::WindowsTelemetryProvider>(
        clock,
        windows::WindowsTelemetryFunctions{
            failed_times, failed_memory, nullptr, failed_io, failed_io});
    CHECK(failed_provider->sample({}, raw).status ==
          telemetry::ProviderSampleStatus::temporarily_failed);
    CHECK(raw.system.cpu_time.status ==
          telemetry::MetricStatus::temporarily_unavailable);
    CHECK(raw.system.memory_total.status ==
          telemetry::MetricStatus::temporarily_unavailable);

    auto recovered_provider = std::make_unique<windows::WindowsTelemetryProvider>(
        clock, fixture_functions(fixture));
    CHECK(recovered_provider->sample({}, raw).status ==
          telemetry::ProviderSampleStatus::complete);
    CHECK(raw.system.cpu_time.has_value());
    CHECK(raw.system.memory_total.has_value());
}

TEST_CASE("Windows provider uses lifecycle-safe synthetic I/O totals",
          "[telemetry][windows]") {
    core::SystemMonotonicClock clock;
    IoFixture fixture;
    windows::WindowsTelemetryProvider provider{clock, fixture_functions(fixture)};
    telemetry::RawTelemetrySnapshot raw;

    REQUIRE(provider.sample({}, raw).status == telemetry::ProviderSampleStatus::complete);
    CHECK(raw.system.disk_read_bytes.value.value == 0U);
    CHECK(raw.system.network_receive_bytes.value.value == 0U);

    fixture.disk[0].first_bytes += 25U;
    fixture.disk[1].first_bytes += 75U;
    fixture.network[0].first_bytes += 50U;
    REQUIRE(provider.sample({}, raw).status == telemetry::ProviderSampleStatus::complete);
    CHECK(raw.system.disk_read_bytes.value.value == 100U);
    CHECK(raw.system.network_receive_bytes.value.value == 50U);

    fixture.disk[0].first_bytes = 1U;
    CHECK(provider.sample({}, raw).status == telemetry::ProviderSampleStatus::partial);
    CHECK(raw.system.disk_read_bytes.status ==
          telemetry::MetricStatus::temporarily_unavailable);
    CHECK(raw.system.disk_write_bytes.has_value());
    CHECK(raw.system.network_receive_bytes.has_value());
}

TEST_CASE("Windows provider preserves connectivity when independent TCP statistics fail",
          "[telemetry][windows][network][quality][degradation]") {
    core::SystemMonotonicClock clock;
    IoFixture fixture;
    auto functions = fixture_functions(fixture);
    functions.read_network_quality = read_partial_network_quality;
    windows::WindowsTelemetryProvider provider{clock, functions};
    telemetry::RawTelemetrySnapshot raw;

    CHECK(provider.sample({}, raw).status ==
          telemetry::ProviderSampleStatus::partial);
    REQUIRE(raw.system.network_quality.connectivity.has_value());
    CHECK(raw.system.network_quality.connectivity.value ==
          telemetry::NetworkConnectivityLevel::local);
    REQUIRE(raw.system.network_quality.interface_change_counter.has_value());
    CHECK(raw.system.network_quality.interface_change_counter.value == 3U);
    CHECK(raw.system.network_quality.tcp_out_segments.status ==
          telemetry::MetricStatus::temporarily_unavailable);
}

TEST_CASE("Windows executable paths are resolved only on the slow tier",
          "[telemetry][windows][process]") {
    core::SystemMonotonicClock clock;
    windows::WindowsTelemetryProvider provider{clock};
    telemetry::RawTelemetrySnapshot raw;

    static_cast<void>(provider.sample({}, raw));
    CHECK(raw.process_diagnostics.metadata_resolved > 0U);
    static_cast<void>(provider.sample(
        telemetry::SamplingRequest{
            telemetry::SamplingTier::fast | telemetry::SamplingTier::normal}, raw));
    CHECK(raw.process_diagnostics.metadata_resolved == 0U);
    CHECK(raw.process_diagnostics.metadata_failures == 0U);
}
