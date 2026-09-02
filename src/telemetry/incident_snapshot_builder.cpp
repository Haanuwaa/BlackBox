#include "telemetry/incident_snapshot_builder.hpp"

#include <algorithm>
#include <vector>

namespace blackbox::telemetry {
namespace {

[[nodiscard]] constexpr core::RecordedValueStatus
recorded_status(const MetricStatus status) noexcept {
    switch (status) {
    case MetricStatus::available:
        return core::RecordedValueStatus::available;
    case MetricStatus::unsupported:
        return core::RecordedValueStatus::unsupported;
    case MetricStatus::inaccessible:
        return core::RecordedValueStatus::inaccessible;
    case MetricStatus::temporarily_unavailable:
        return core::RecordedValueStatus::temporarily_unavailable;
    }
    return core::RecordedValueStatus::temporarily_unavailable;
}

template <typename Destination, typename Source, typename Conversion>
[[nodiscard]] Destination recorded_value(const MetricValue<Source>& source, Conversion convert) {
    Destination result{};
    result.status = recorded_status(source.status);
    if (source.has_value()) {
        result.value = convert(source.value);
    }
    return result;
}

template <typename T> [[nodiscard]] T identity_value(const T value) noexcept { return value; }

[[nodiscard]] core::IncidentProcessIdentity
recorded_identity(const ProcessIdentity identity) noexcept {
    return {identity.pid.value, identity.creation_token};
}

[[nodiscard]] core::IncidentApplicationIdentity
recorded_identity(const OpaqueApplicationIdentity identity) noexcept {
    return {identity.session_token, identity.application_token};
}

[[nodiscard]] core::IncidentSystemSample recorded_system_sample(const SystemSample& sample) {
    core::IncidentSystemSample result{};
    result.observed_at = sample.observed_at;
    result.cpu_fraction = recorded_value<core::RecordedValue<double>>(
        sample.cpu_usage, [](const Ratio value) { return value.value; });
    result.memory_used_bytes = recorded_value<core::RecordedValue<std::uint64_t>>(
        sample.memory_used, [](const ByteCount value) { return value.value; });
    result.memory_total_bytes = recorded_value<core::RecordedValue<std::uint64_t>>(
        sample.memory_total, [](const ByteCount value) { return value.value; });
    result.memory_fraction = recorded_value<core::RecordedValue<double>>(
        sample.memory_usage, [](const Ratio value) { return value.value; });
    result.disk_read_bytes_per_second = recorded_value<core::RecordedValue<double>>(
        sample.disk_read_rate, [](const BytesPerSecond value) { return value.value; });
    result.disk_write_bytes_per_second = recorded_value<core::RecordedValue<double>>(
        sample.disk_write_rate, [](const BytesPerSecond value) { return value.value; });
    result.network_receive_bytes_per_second = recorded_value<core::RecordedValue<double>>(
        sample.network_receive_rate, [](const BytesPerSecond value) { return value.value; });
    result.network_transmit_bytes_per_second = recorded_value<core::RecordedValue<double>>(
        sample.network_transmit_rate, [](const BytesPerSecond value) { return value.value; });
    result.disk_read_latency_seconds = recorded_value<core::RecordedValue<double>>(
        sample.disk_read_latency, [](const Seconds value) { return value.value; });
    result.disk_write_latency_seconds = recorded_value<core::RecordedValue<double>>(
        sample.disk_write_latency, [](const Seconds value) { return value.value; });
    result.disk_service_time_seconds = recorded_value<core::RecordedValue<double>>(
        sample.disk_service_time, [](const Seconds value) { return value.value; });
    result.disk_queue_depth = recorded_value<core::RecordedValue<double>>(sample.disk_queue_depth,
                                                                          identity_value<double>);
    result.disk_service_concurrency = recorded_value<core::RecordedValue<double>>(
        sample.disk_service_concurrency, identity_value<double>);
    result.disk_worst_device_id = recorded_value<core::RecordedValue<std::uint64_t>>(
        sample.disk_worst_device_id, identity_value<std::uint64_t>);
    result.compressed_memory_bytes = recorded_value<core::RecordedValue<std::uint64_t>>(
        sample.compressed_memory, [](const ByteCount value) { return value.value; });
    result.memory_page_out_bytes_per_second = recorded_value<core::RecordedValue<double>>(
        sample.memory_page_out_rate, [](const BytesPerSecond value) { return value.value; });
    result.memory_swap_in_bytes_per_second = recorded_value<core::RecordedValue<double>>(
        sample.memory_swap_in_rate, [](const BytesPerSecond value) { return value.value; });
    result.memory_swap_out_bytes_per_second = recorded_value<core::RecordedValue<double>>(
        sample.memory_swap_out_rate, [](const BytesPerSecond value) { return value.value; });
    result.memory_compression_bytes_per_second = recorded_value<core::RecordedValue<double>>(
        sample.memory_compression_rate, [](const BytesPerSecond value) { return value.value; });
    result.memory_decompression_bytes_per_second = recorded_value<core::RecordedValue<double>>(
        sample.memory_decompression_rate, [](const BytesPerSecond value) { return value.value; });
    result.network_connectivity_level = recorded_value<core::RecordedValue<std::uint8_t>>(
        sample.network_connectivity,
        [](const NetworkConnectivityLevel value) { return static_cast<std::uint8_t>(value); });
    result.network_active_interfaces = recorded_value<core::RecordedValue<std::uint64_t>>(
        sample.network_active_interfaces, identity_value<std::uint64_t>);
    result.network_interface_changes = recorded_value<core::RecordedValue<std::uint64_t>>(
        sample.network_interface_changes, identity_value<std::uint64_t>);
    result.network_tcp_retransmit_fraction = recorded_value<core::RecordedValue<double>>(
        sample.network_tcp_retransmit_fraction, [](const Ratio value) { return value.value; });
    result.network_tcp_failed_connections = recorded_value<core::RecordedValue<std::uint64_t>>(
        sample.network_tcp_failed_connections, identity_value<std::uint64_t>);
    result.network_tcp_resets = recorded_value<core::RecordedValue<std::uint64_t>>(
        sample.network_tcp_resets, identity_value<std::uint64_t>);
    result.gpu_fraction = recorded_value<core::RecordedValue<double>>(
        sample.gpu_usage, [](const Ratio value) { return value.value; });
    result.gpu_dedicated_memory_bytes = recorded_value<core::RecordedValue<std::uint64_t>>(
        sample.gpu_dedicated_memory, [](const ByteCount value) { return value.value; });
    result.gpu_shared_memory_bytes = recorded_value<core::RecordedValue<std::uint64_t>>(
        sample.gpu_shared_memory, [](const ByteCount value) { return value.value; });
    result.foreground_process = recorded_value<core::RecordedValue<core::IncidentProcessIdentity>>(
        sample.foreground_process,
        [](const ProcessIdentity value) { return recorded_identity(value); });
    result.foreground_application =
        recorded_value<core::RecordedValue<core::IncidentApplicationIdentity>>(
            sample.foreground_application,
            [](const OpaqueApplicationIdentity value) { return recorded_identity(value); });
    result.foreground_gpu_fraction = recorded_value<core::RecordedValue<double>>(
        sample.foreground_gpu_usage, [](const Ratio value) { return value.value; });
    result.dpc_fraction = recorded_value<core::RecordedValue<double>>(
        sample.dpc_usage, [](const Ratio value) { return value.value; });
    result.interrupt_fraction = recorded_value<core::RecordedValue<double>>(
        sample.interrupt_usage, [](const Ratio value) { return value.value; });
    result.dpc_rate =
        recorded_value<core::RecordedValue<double>>(sample.dpc_rate, identity_value<double>);
    result.cpu_current_mhz =
        recorded_value<core::RecordedValue<double>>(sample.cpu_current_mhz, identity_value<double>);
    result.cpu_max_mhz =
        recorded_value<core::RecordedValue<double>>(sample.cpu_max_mhz, identity_value<double>);
    result.cpu_thermal_limit_mhz = recorded_value<core::RecordedValue<double>>(
        sample.cpu_thermal_limit_mhz, identity_value<double>);
    result.cpu_thermal_limit_fraction = recorded_value<core::RecordedValue<double>>(
        sample.cpu_thermal_limit_fraction, [](const Ratio value) { return value.value; });
    result.power_source = recorded_value<core::RecordedValue<std::uint8_t>>(
        sample.power_source,
        [](const PowerSource value) { return static_cast<std::uint8_t>(value); });
    result.battery_fraction = recorded_value<core::RecordedValue<double>>(
        sample.battery_fraction, [](const Ratio value) { return value.value; });
    result.battery_saver =
        recorded_value<core::RecordedValue<bool>>(sample.battery_saver, identity_value<bool>);
    result.system_uptime_seconds = recorded_value<core::RecordedValue<double>>(
        sample.system_uptime, [](const Seconds value) { return value.value; });
    result.cpu_some_pressure_fraction = recorded_value<core::RecordedValue<double>>(
        sample.cpu_some_pressure, [](const Ratio value) { return value.value; });
    result.memory_some_pressure_fraction = recorded_value<core::RecordedValue<double>>(
        sample.memory_some_pressure, [](const Ratio value) { return value.value; });
    result.memory_full_pressure_fraction = recorded_value<core::RecordedValue<double>>(
        sample.memory_full_pressure, [](const Ratio value) { return value.value; });
    result.io_some_pressure_fraction = recorded_value<core::RecordedValue<double>>(
        sample.io_some_pressure, [](const Ratio value) { return value.value; });
    result.io_full_pressure_fraction = recorded_value<core::RecordedValue<double>>(
        sample.io_full_pressure, [](const Ratio value) { return value.value; });
    result.thermal_pressure_state = recorded_value<core::RecordedValue<std::uint8_t>>(
        sample.thermal_pressure_state,
        [](const ThermalPressureState value) { return static_cast<std::uint8_t>(value); });
    result.memory_pressure_state = recorded_value<core::RecordedValue<std::uint8_t>>(
        sample.memory_pressure_state,
        [](const MemoryPressureState value) { return static_cast<std::uint8_t>(value); });
    result.scheduler_delay_seconds = recorded_value<core::RecordedValue<double>>(
        sample.scheduler_delay, [](const Seconds value) { return value.value; });
    result.logical_processor_count = recorded_value<core::RecordedValue<std::uint32_t>>(
        sample.logical_processor_count, identity_value<std::uint32_t>);
    result.physical_processor_count = recorded_value<core::RecordedValue<std::uint32_t>>(
        sample.physical_processor_count, identity_value<std::uint32_t>);
    result.active_processor_count = recorded_value<core::RecordedValue<std::uint32_t>>(
        sample.active_processor_count, identity_value<std::uint32_t>);
    return result;
}

[[nodiscard]] core::IncidentProcessSample
recorded_process_sample(const core::MonotonicTimePoint observed_at, const ProcessSample& sample) {
    core::IncidentProcessSample result{};
    result.observed_at = observed_at;
    result.identity = recorded_identity(sample.identity);
    result.cpu_fraction = recorded_value<core::RecordedValue<double>>(
        sample.cpu_usage, [](const Ratio value) { return value.value; });
    result.working_set_bytes = recorded_value<core::RecordedValue<std::uint64_t>>(
        sample.working_set, [](const ByteCount value) { return value.value; });
    result.disk_read_bytes_per_second = recorded_value<core::RecordedValue<double>>(
        sample.disk_read_rate, [](const BytesPerSecond value) { return value.value; });
    result.disk_write_bytes_per_second = recorded_value<core::RecordedValue<double>>(
        sample.disk_write_rate, [](const BytesPerSecond value) { return value.value; });
    return result;
}

[[nodiscard]] core::IncidentProcessInfo recorded_process_info(const ProcessInfo& info) {
    core::IncidentProcessInfo result{};
    result.identity = recorded_identity(info.identity);
    result.parent_pid = recorded_value<core::RecordedValue<std::uint32_t>>(
        info.parent_pid, [](const ProcessId value) { return value.value; });
    result.name =
        recorded_value<core::RecordedValue<std::string>>(info.name, identity_value<std::string>);
    result.executable_path = recorded_value<core::RecordedValue<std::string>>(
        info.executable_path, identity_value<std::string>);
    return result;
}

} // namespace

std::shared_ptr<const core::IncidentSnapshot>
build_incident_snapshot(const core::IncidentCaptureWindow& window,
                        const core::MonotonicTimePoint completion_observed_at,
                        const core::RecorderSnapshot<SystemSample>& system_history,
                        const core::RecorderSnapshot<ProcessFrame>& process_history,
                        const std::span<const ProcessInfo> metadata,
                        const core::RecorderSnapshot<core::SystemEvent>* event_history) {
    std::vector<core::IncidentSystemSample> system_samples;
    system_samples.reserve(system_history.size());
    for (const auto& sample : system_history.samples()) {
        if (sample.observed_at >= window.requested_start &&
            sample.observed_at <= completion_observed_at) {
            system_samples.push_back(recorded_system_sample(sample));
        }
    }

    std::vector<core::IncidentProcessSample> process_samples;
    std::vector<core::IncidentProcessIdentity> referenced_identities;
    std::size_t selected_process_samples = 0U;
    for (const auto& frame : process_history.samples()) {
        if (frame.observed_at >= window.requested_start &&
            frame.observed_at <= completion_observed_at) {
            selected_process_samples += frame.processes.size();
        }
    }
    process_samples.reserve(selected_process_samples);
    referenced_identities.reserve(selected_process_samples);
    for (const auto& frame : process_history.samples()) {
        if (frame.observed_at < window.requested_start ||
            frame.observed_at > completion_observed_at) {
            continue;
        }
        for (const auto& sample : frame.processes) {
            auto recorded = recorded_process_sample(frame.observed_at, sample);
            referenced_identities.push_back(recorded.identity);
            process_samples.push_back(std::move(recorded));
        }
    }

    std::vector<core::SystemEvent> system_events;
    if (event_history != nullptr) {
        system_events.reserve(event_history->size());
        for (const auto& event : event_history->samples()) {
            if (event.observed_at >= window.requested_start &&
                event.observed_at <= completion_observed_at) {
                system_events.push_back(event);
                if (event.has_process_identity) {
                    referenced_identities.push_back(
                        {event.process_pid, event.process_creation_token});
                }
            }
        }
    }

    std::sort(referenced_identities.begin(), referenced_identities.end());
    referenced_identities.erase(
        std::unique(referenced_identities.begin(), referenced_identities.end()),
        referenced_identities.end());

    std::vector<core::IncidentProcessInfo> process_metadata;
    process_metadata.reserve(std::min(metadata.size(), referenced_identities.size()));
    for (const auto& info : metadata) {
        const auto identity = recorded_identity(info.identity);
        if (std::binary_search(referenced_identities.begin(), referenced_identities.end(),
                               identity)) {
            process_metadata.push_back(recorded_process_info(info));
        }
    }
    std::sort(process_metadata.begin(), process_metadata.end(),
              [](const core::IncidentProcessInfo& left, const core::IncidentProcessInfo& right) {
                  return left.identity < right.identity;
              });

    core::IncidentHeader header{};
    header.window = window;
    header.system_recorder_epoch = system_history.epoch();
    header.process_recorder_epoch = process_history.epoch();
    header.event_recorder_epoch = event_history != nullptr ? event_history->epoch() : 0U;
    if (!system_samples.empty()) {
        header.actual_start = system_samples.front().observed_at;
        header.actual_end = system_samples.back().observed_at;
    } else if (!process_samples.empty()) {
        header.actual_start = process_samples.front().observed_at;
        header.actual_end = process_samples.back().observed_at;
    } else if (!system_events.empty()) {
        header.actual_start = system_events.front().observed_at;
        header.actual_end = system_events.back().observed_at;
    } else {
        header.actual_start = completion_observed_at;
        header.actual_end = completion_observed_at;
    }

    return std::make_shared<const core::IncidentSnapshot>(
        std::move(header), std::move(system_samples), std::move(process_metadata),
        std::move(process_samples), std::move(system_events));
}

} // namespace blackbox::telemetry
