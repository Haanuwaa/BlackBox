#include "telemetry/provider.hpp"

#include <cstddef>
#include <cmath>

namespace blackbox::telemetry {
namespace {

template <typename T>
[[nodiscard]] bool unsupported_when_absent(const bool capability,
                                           const MetricValue<T>& metric) noexcept {
    return capability || metric.status == MetricStatus::unsupported;
}

} // namespace

ProviderContractViolation validate_provider_snapshot_contract(
    const PlatformCapabilities& capabilities,
    const SamplingRequest request,
    const RawTelemetrySnapshot& snapshot) noexcept {
    if (snapshot.sampled_tiers != request.tiers) {
        return ProviderContractViolation::sampled_tiers_mismatch;
    }
    if (!unsupported_when_absent(capabilities.cpu_usage, snapshot.system.cpu_time) ||
        !unsupported_when_absent(capabilities.memory_usage, snapshot.system.memory_total) ||
        !unsupported_when_absent(capabilities.memory_usage, snapshot.system.memory_available) ||
        !unsupported_when_absent(capabilities.disk_throughput,
                                 snapshot.system.disk_read_bytes) ||
        !unsupported_when_absent(capabilities.disk_throughput,
                                 snapshot.system.disk_write_bytes) ||
        !unsupported_when_absent(capabilities.disk_latency,
                                 snapshot.system.disk_quality.read_latency) ||
        !unsupported_when_absent(capabilities.disk_latency,
                                 snapshot.system.disk_quality.write_latency) ||
        !unsupported_when_absent(capabilities.disk_service_time,
                                 snapshot.system.disk_quality.service_time) ||
        !unsupported_when_absent(capabilities.disk_queue_depth,
                                 snapshot.system.disk_quality.queue_depth) ||
        !unsupported_when_absent(capabilities.disk_latency ||
                                     capabilities.disk_service_time ||
                                     capabilities.disk_queue_depth,
                                 snapshot.system.disk_quality.worst_device_id) ||
        !unsupported_when_absent(capabilities.network_usage,
                                 snapshot.system.network_receive_bytes) ||
        !unsupported_when_absent(capabilities.network_usage,
                                 snapshot.system.network_transmit_bytes) ||
        !unsupported_when_absent(capabilities.network_connectivity,
                                 snapshot.system.network_quality.connectivity) ||
        !unsupported_when_absent(capabilities.network_connectivity,
                                 snapshot.system.network_quality.active_interfaces) ||
        !unsupported_when_absent(capabilities.network_connectivity,
                                 snapshot.system.network_quality.interface_change_counter) ||
        !unsupported_when_absent(capabilities.network_transport_quality,
                                 snapshot.system.network_quality.tcp_out_segments) ||
        !unsupported_when_absent(capabilities.network_transport_quality,
                                 snapshot.system.network_quality.tcp_retransmitted_segments) ||
        !unsupported_when_absent(capabilities.network_transport_quality,
                                 snapshot.system.network_quality.tcp_failed_connections) ||
        !unsupported_when_absent(capabilities.network_transport_quality,
                                 snapshot.system.network_quality.tcp_established_resets)) {
        return ProviderContractViolation::capability_status_mismatch;
    }
    if (!unsupported_when_absent(capabilities.gpu_usage, snapshot.system.gpu_usage) ||
        !unsupported_when_absent(capabilities.gpu_memory,
                                 snapshot.system.gpu_dedicated_memory) ||
        !unsupported_when_absent(capabilities.gpu_memory,
                                 snapshot.system.gpu_shared_memory) ||
        !unsupported_when_absent(capabilities.foreground_application,
                                 snapshot.system.foreground_process) ||
        !unsupported_when_absent(capabilities.foreground_gpu_usage,
                                 snapshot.system.foreground_gpu_usage) ||
        !unsupported_when_absent(capabilities.dpc_isr, snapshot.system.dpc_usage) ||
        !unsupported_when_absent(capabilities.dpc_isr, snapshot.system.interrupt_usage) ||
        !unsupported_when_absent(capabilities.dpc_isr, snapshot.system.dpc_rate) ||
        !unsupported_when_absent(capabilities.cpu_frequency,
                                 snapshot.system.cpu_current_mhz) ||
        !unsupported_when_absent(capabilities.cpu_frequency,
                                 snapshot.system.cpu_max_mhz) ||
        !unsupported_when_absent(capabilities.cpu_thermal_limit,
                                 snapshot.system.cpu_thermal_limit_mhz) ||
        !unsupported_when_absent(capabilities.cpu_thermal_limit,
                                 snapshot.system.cpu_thermal_limit_fraction) ||
        !unsupported_when_absent(capabilities.power_status,
                                 snapshot.system.power_source) ||
        !unsupported_when_absent(capabilities.power_status,
                                 snapshot.system.battery_fraction) ||
        !unsupported_when_absent(capabilities.power_status,
                                 snapshot.system.battery_saver) ||
        !unsupported_when_absent(capabilities.system_uptime,
                                 snapshot.system.system_uptime)) {
        return ProviderContractViolation::capability_status_mismatch;
    }
    if (snapshot.system.cpu_time.has_value() &&
        (snapshot.system.cpu_time.value.total_ticks == 0U ||
         snapshot.system.cpu_time.value.busy_ticks >
             snapshot.system.cpu_time.value.total_ticks)) {
        return ProviderContractViolation::invalid_cpu_counters;
    }
    if (snapshot.system.memory_total.has_value() &&
        (snapshot.system.memory_total.value.value == 0U ||
         (snapshot.system.memory_available.has_value() &&
          snapshot.system.memory_available.value.value >
              snapshot.system.memory_total.value.value))) {
        return ProviderContractViolation::invalid_memory_counters;
    }
    const auto valid_nonnegative = [](const auto& value) {
        return !value.has_value() ||
               (std::isfinite(value.value.value) && value.value.value >= 0.0);
    };
    if (!valid_nonnegative(snapshot.system.disk_quality.read_latency) ||
        !valid_nonnegative(snapshot.system.disk_quality.write_latency) ||
        !valid_nonnegative(snapshot.system.disk_quality.service_time) ||
        (snapshot.system.disk_quality.queue_depth.has_value() &&
         (!std::isfinite(snapshot.system.disk_quality.queue_depth.value) ||
          snapshot.system.disk_quality.queue_depth.value < 0.0))) {
        return ProviderContractViolation::invalid_disk_quality;
    }
    if (snapshot.system.network_quality.connectivity.has_value() &&
        snapshot.system.network_quality.connectivity.value >
            NetworkConnectivityLevel::unknown) {
        return ProviderContractViolation::invalid_network_quality;
    }
    const auto valid_ratio = [](const MetricValue<Ratio>& value) {
        return !value.has_value() ||
               (std::isfinite(value.value.value) && value.value.value >= 0.0 &&
                value.value.value <= 1.0);
    };
    if (!valid_ratio(snapshot.system.gpu_usage) ||
        !valid_ratio(snapshot.system.foreground_gpu_usage)) {
        return ProviderContractViolation::invalid_gpu_evidence;
    }
    if (!valid_ratio(snapshot.system.dpc_usage) ||
        !valid_ratio(snapshot.system.interrupt_usage) ||
        (snapshot.system.dpc_rate.has_value() &&
         (!std::isfinite(snapshot.system.dpc_rate.value) ||
          snapshot.system.dpc_rate.value < 0.0))) {
        return ProviderContractViolation::invalid_responsiveness_evidence;
    }
    const auto valid_positive = [](const MetricValue<double>& value) {
        return !value.has_value() ||
               (std::isfinite(value.value) && value.value > 0.0);
    };
    if (!valid_positive(snapshot.system.cpu_current_mhz) ||
        !valid_positive(snapshot.system.cpu_max_mhz) ||
        !valid_positive(snapshot.system.cpu_thermal_limit_mhz) ||
        !valid_ratio(snapshot.system.cpu_thermal_limit_fraction) ||
        !valid_ratio(snapshot.system.battery_fraction) ||
        (snapshot.system.power_source.has_value() &&
         snapshot.system.power_source.value > PowerSource::unknown) ||
        (snapshot.system.system_uptime.has_value() &&
         (!std::isfinite(snapshot.system.system_uptime.value.value) ||
          snapshot.system.system_uptime.value.value < 0.0))) {
        return ProviderContractViolation::invalid_power_evidence;
    }
    if (snapshot.system.foreground_process.has_value() &&
        (snapshot.system.foreground_process.value.pid.value == 0U ||
         snapshot.system.foreground_process.value.creation_token == 0U)) {
        return ProviderContractViolation::invalid_process_identity;
    }
    for (std::size_t index = 0U; index < snapshot.processes.size(); ++index) {
        const auto& process = snapshot.processes[index];
        if (process.identity.pid.value == 0U || process.identity.creation_token == 0U) {
            return ProviderContractViolation::invalid_process_identity;
        }
        if (!unsupported_when_absent(capabilities.process_cpu, process.cpu_time) ||
            !unsupported_when_absent(capabilities.process_memory, process.working_set) ||
            !unsupported_when_absent(capabilities.process_disk_io,
                                     process.disk_read_bytes) ||
            !unsupported_when_absent(capabilities.process_disk_io,
                                     process.disk_write_bytes)) {
            return ProviderContractViolation::capability_status_mismatch;
        }
        for (std::size_t previous = 0U; previous < index; ++previous) {
            if (snapshot.processes[previous].identity == process.identity) {
                return ProviderContractViolation::duplicate_process_identity;
            }
        }
    }
    return ProviderContractViolation::none;
}

} // namespace blackbox::telemetry
