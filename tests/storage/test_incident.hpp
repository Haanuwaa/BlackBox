#pragma once

#include "core/incident.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace blackbox::storage::test {

using namespace std::chrono_literals;

[[nodiscard]] inline std::shared_ptr<const core::IncidentSnapshot>
representative_incident(const std::size_t large_text_bytes = 0U) {
    core::IncidentHeader header{};
    header.window.sequence = std::numeric_limits<std::uint64_t>::max();
    header.window.event_time = core::MonotonicTimePoint{100s};
    header.window.requested_start = core::MonotonicTimePoint{-20s};
    header.window.requested_end = core::MonotonicTimePoint{130s};
    header.window.trigger_count = 2U;
    header.window.manual_trigger_count = 1U;
    header.window.automatic_trigger_count = 1U;
    header.window.automatic_resource = core::AutomaticIncidentResource::cpu;
    header.window.automatic_signal = core::AutomaticIncidentSignal::disk_latency;
    header.window.automatic_observed_value = 0.99;
    header.window.automatic_baseline_value = 0.35;
    header.window.automatic_score = 2.5;
    header.actual_start = core::MonotonicTimePoint{5s};
    header.actual_end = core::MonotonicTimePoint{130s};
    header.system_recorder_epoch = std::numeric_limits<std::uint64_t>::max() - 1U;
    header.process_recorder_epoch = 42U;
    header.event_recorder_epoch = 77U;

    core::IncidentSystemSample first{};
    first.observed_at = core::MonotonicTimePoint{5s};
    first.cpu_fraction = {0.75, core::RecordedValueStatus::available};
    first.memory_used_bytes = {std::numeric_limits<std::uint64_t>::max(),
                               core::RecordedValueStatus::available};
    first.memory_total_bytes.status = core::RecordedValueStatus::inaccessible;
    first.memory_fraction = {0.5, core::RecordedValueStatus::available};
    first.disk_read_bytes_per_second = {1024.5, core::RecordedValueStatus::available};
    first.disk_write_bytes_per_second.status = core::RecordedValueStatus::temporarily_unavailable;
    first.network_receive_bytes_per_second.status = core::RecordedValueStatus::unsupported;
    first.network_transmit_bytes_per_second = {2048.25, core::RecordedValueStatus::available};
    first.disk_read_latency_seconds = {0.012, core::RecordedValueStatus::available};
    first.disk_write_latency_seconds.status = core::RecordedValueStatus::temporarily_unavailable;
    first.disk_service_time_seconds = {0.018, core::RecordedValueStatus::available};
    first.disk_queue_depth = {3.5, core::RecordedValueStatus::available};
    first.disk_worst_device_id = {7U, core::RecordedValueStatus::available};
    first.network_connectivity_level = {2U, core::RecordedValueStatus::available};
    first.network_active_interfaces = {1U, core::RecordedValueStatus::available};
    first.network_interface_changes = {2U, core::RecordedValueStatus::available};
    first.network_tcp_retransmit_fraction = {0.125, core::RecordedValueStatus::available};
    first.network_tcp_failed_connections = {3U, core::RecordedValueStatus::available};
    first.network_tcp_resets.status = core::RecordedValueStatus::inaccessible;
    first.gpu_fraction = {0.8, core::RecordedValueStatus::available};
    first.gpu_dedicated_memory_bytes = {3ULL << 30U, core::RecordedValueStatus::available};
    first.gpu_shared_memory_bytes = {512ULL << 20U, core::RecordedValueStatus::available};
    first.foreground_process = {{99U, 123'456U}, core::RecordedValueStatus::available};
    first.foreground_gpu_fraction = {0.7, core::RecordedValueStatus::available};
    first.dpc_fraction = {0.03, core::RecordedValueStatus::available};
    first.interrupt_fraction = {0.02, core::RecordedValueStatus::available};
    first.dpc_rate = {1'234.5, core::RecordedValueStatus::available};
    first.cpu_current_mhz = {3'200.0, core::RecordedValueStatus::available};
    first.cpu_max_mhz = {4'500.0, core::RecordedValueStatus::available};
    first.cpu_thermal_limit_mhz = {3'600.0, core::RecordedValueStatus::available};
    first.cpu_thermal_limit_fraction = {0.8, core::RecordedValueStatus::available};
    first.power_source = {1U, core::RecordedValueStatus::available};
    first.battery_fraction = {0.42, core::RecordedValueStatus::available};
    first.battery_saver = {true, core::RecordedValueStatus::available};
    first.system_uptime_seconds = {98'765.25, core::RecordedValueStatus::available};
    first.cpu_some_pressure_fraction = {0.15, core::RecordedValueStatus::available};
    first.memory_some_pressure_fraction = {0.25, core::RecordedValueStatus::available};
    first.memory_full_pressure_fraction.status = core::RecordedValueStatus::temporarily_unavailable;
    first.io_some_pressure_fraction = {0.35, core::RecordedValueStatus::available};
    first.io_full_pressure_fraction.status = core::RecordedValueStatus::inaccessible;
    first.thermal_pressure_state = {2U, core::RecordedValueStatus::available};
    auto second = first;
    second.observed_at = core::MonotonicTimePoint{130s};
    second.cpu_fraction.value = 0.25;

    const core::IncidentProcessIdentity identity{std::numeric_limits<std::uint32_t>::max(),
                                                 std::numeric_limits<std::uint64_t>::max()};
    core::IncidentProcessInfo info{};
    info.identity = identity;
    info.parent_pid = {123U, core::RecordedValueStatus::available};
    info.name = {large_text_bytes == 0U ? std::string{"fixture.exe"}
                                        : std::string(large_text_bytes, 'x'),
                 core::RecordedValueStatus::available};
    info.executable_path = {"C:\\Fixture\\fixture.exe", core::RecordedValueStatus::available};

    core::IncidentProcessSample process{};
    process.observed_at = core::MonotonicTimePoint{130s};
    process.identity = identity;
    process.cpu_fraction = {0.125, core::RecordedValueStatus::available};
    process.working_set_bytes = {std::numeric_limits<std::uint64_t>::max(),
                                 core::RecordedValueStatus::available};
    process.disk_read_bytes_per_second.status = core::RecordedValueStatus::inaccessible;
    process.disk_write_bytes_per_second = {4096.75, core::RecordedValueStatus::available};

    core::SystemEvent event{};
    event.observed_at = core::MonotonicTimePoint{99s};
    event.source_utc_milliseconds = 1'700'000'000'123;
    event.has_source_utc_time = true;
    event.source = core::SystemEventSource::network;
    event.kind = core::SystemEventKind::dns_resolution_timeout;
    event.level = core::SystemEventLevel::error;
    event.native_event_id = 1014U;
    event.detail = 9U;

    return std::make_shared<const core::IncidentSnapshot>(
        std::move(header), std::vector{first, second}, std::vector{std::move(info)},
        std::vector{process}, std::vector{event});
}

[[nodiscard]] inline std::shared_ptr<const core::IncidentSnapshot>
scaled_incident(const std::size_t process_count, const std::size_t frame_count,
                const bool inject_system_spike = true) {
    core::IncidentHeader header{};
    header.window.sequence = 7U;
    header.window.event_time =
        core::MonotonicTimePoint{std::chrono::seconds{static_cast<std::int64_t>(frame_count / 2U)}};
    header.window.requested_start = core::MonotonicTimePoint{0s};
    header.window.requested_end =
        core::MonotonicTimePoint{std::chrono::seconds{static_cast<std::int64_t>(frame_count)}};
    header.actual_start = header.window.requested_start;
    header.actual_end =
        core::MonotonicTimePoint{std::chrono::seconds{static_cast<std::int64_t>(frame_count - 1U)}};

    std::vector<core::IncidentSystemSample> systems;
    std::vector<core::IncidentProcessInfo> metadata;
    std::vector<core::IncidentProcessSample> processes;
    systems.reserve(frame_count);
    metadata.reserve(process_count);
    processes.reserve(process_count * frame_count);
    for (std::size_t process_index = 0U; process_index < process_count; ++process_index) {
        core::IncidentProcessInfo info{};
        info.identity = {static_cast<std::uint32_t>(process_index + 1U), process_index + 1'000U};
        info.name = {"process-" + std::to_string(process_index) + ".exe",
                     core::RecordedValueStatus::available};
        info.executable_path = {"C:\\Fixture\\" + info.name.value,
                                core::RecordedValueStatus::available};
        metadata.push_back(std::move(info));
    }
    for (std::size_t frame = 0U; frame < frame_count; ++frame) {
        core::IncidentSystemSample system{};
        system.observed_at =
            core::MonotonicTimePoint{std::chrono::seconds{static_cast<std::int64_t>(frame)}};
        system.cpu_fraction = {inject_system_spike && frame == frame_count / 2U ? 1.0 : 0.25,
                               core::RecordedValueStatus::available};
        system.memory_fraction = {0.5, core::RecordedValueStatus::available};
        system.disk_read_bytes_per_second = {1'048'576.0, core::RecordedValueStatus::available};
        systems.push_back(system);
        for (const auto& info : metadata) {
            core::IncidentProcessSample sample{};
            sample.observed_at = system.observed_at;
            sample.identity = info.identity;
            sample.cpu_fraction = {static_cast<double>(info.identity.pid % 100U) / 100.0,
                                   core::RecordedValueStatus::available};
            sample.working_set_bytes = {static_cast<std::uint64_t>(info.identity.pid) << 20U,
                                        core::RecordedValueStatus::available};
            sample.disk_read_bytes_per_second = {static_cast<double>(info.identity.pid) * 1024.0,
                                                 core::RecordedValueStatus::available};
            processes.push_back(sample);
        }
    }
    return std::make_shared<const core::IncidentSnapshot>(
        std::move(header), std::move(systems), std::move(metadata), std::move(processes));
}

} // namespace blackbox::storage::test
