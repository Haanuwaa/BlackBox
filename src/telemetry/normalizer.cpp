#include "telemetry/normalizer.hpp"

#include <algorithm>

namespace blackbox::telemetry {
namespace {

template <typename T>
[[nodiscard]] constexpr MetricValue<T> propagated_unavailable(const MetricStatus status) noexcept {
    return MetricValue<T>::unavailable(status);
}

template <typename Current, typename Output>
[[nodiscard]] constexpr MetricValue<Output>
current_status_or_temporary(const MetricValue<Current>& previous,
                            const MetricValue<Current>& current) noexcept {
    if (!current.has_value()) {
        return propagated_unavailable<Output>(current.status);
    }
    if (!previous.has_value()) {
        return propagated_unavailable<Output>(MetricStatus::temporarily_unavailable);
    }
    return propagated_unavailable<Output>(MetricStatus::available);
}

template <typename Current, typename Output>
[[nodiscard]] constexpr MetricValue<Output>
initial_cumulative_status(const MetricValue<Current>& current) noexcept {
    if (!current.has_value()) {
        return propagated_unavailable<Output>(current.status);
    }
    return propagated_unavailable<Output>(MetricStatus::temporarily_unavailable);
}

[[nodiscard]] constexpr MetricStatus combined_current_status(const MetricStatus first,
                                                             const MetricStatus second) noexcept {
    if (first != MetricStatus::available) {
        return first;
    }
    return second;
}

[[nodiscard]] constexpr bool
valid_elapsed(const std::chrono::steady_clock::duration elapsed) noexcept {
    return elapsed > std::chrono::steady_clock::duration::zero();
}

} // namespace

MetricValue<Ratio> normalize_cpu_usage(const MetricValue<CpuTimeCounters>& previous,
                                       const MetricValue<CpuTimeCounters>& current,
                                       const std::chrono::steady_clock::duration elapsed) noexcept {
    if (!current.has_value() || !previous.has_value()) {
        return current_status_or_temporary<CpuTimeCounters, Ratio>(previous, current);
    }
    if (!valid_elapsed(elapsed) || current.value.busy_ticks < previous.value.busy_ticks ||
        current.value.total_ticks < previous.value.total_ticks) {
        return MetricValue<Ratio>::unavailable(MetricStatus::temporarily_unavailable);
    }

    const auto busy_delta = current.value.busy_ticks - previous.value.busy_ticks;
    const auto total_delta = current.value.total_ticks - previous.value.total_ticks;
    if (total_delta == 0U || busy_delta > total_delta) {
        return MetricValue<Ratio>::unavailable(MetricStatus::temporarily_unavailable);
    }

    return MetricValue<Ratio>::available(
        Ratio{static_cast<double>(busy_delta) / static_cast<double>(total_delta)});
}

MetricValue<BytesPerSecond>
normalize_byte_rate(const MetricValue<ByteCount>& previous, const MetricValue<ByteCount>& current,
                    const std::chrono::steady_clock::duration elapsed) noexcept {
    if (!current.has_value() || !previous.has_value()) {
        return current_status_or_temporary<ByteCount, BytesPerSecond>(previous, current);
    }
    if (!valid_elapsed(elapsed) || current.value < previous.value) {
        return MetricValue<BytesPerSecond>::unavailable(MetricStatus::temporarily_unavailable);
    }

    const std::chrono::duration<double> seconds{elapsed};
    const auto delta = current.value.value - previous.value.value;
    return MetricValue<BytesPerSecond>::available(
        BytesPerSecond{static_cast<double>(delta) / seconds.count()});
}

MetricValue<std::uint64_t>
normalize_counter_delta(const MetricValue<std::uint64_t>& previous,
                        const MetricValue<std::uint64_t>& current) noexcept {
    if (!current.has_value() || !previous.has_value()) {
        return current_status_or_temporary<std::uint64_t, std::uint64_t>(previous, current);
    }
    if (current.value < previous.value) {
        return MetricValue<std::uint64_t>::unavailable(MetricStatus::temporarily_unavailable);
    }
    return MetricValue<std::uint64_t>::available(current.value - previous.value);
}

MetricValue<Ratio> normalize_tcp_retransmit_fraction(
    const MetricValue<std::uint64_t>& previous_out, const MetricValue<std::uint64_t>& current_out,
    const MetricValue<std::uint64_t>& previous_retransmitted,
    const MetricValue<std::uint64_t>& current_retransmitted) noexcept {
    const auto out = normalize_counter_delta(previous_out, current_out);
    if (!out.has_value()) {
        return MetricValue<Ratio>::unavailable(out.status);
    }
    const auto retransmitted =
        normalize_counter_delta(previous_retransmitted, current_retransmitted);
    if (!retransmitted.has_value()) {
        return MetricValue<Ratio>::unavailable(retransmitted.status);
    }
    const auto total = out.value + retransmitted.value;
    if (total == 0U) {
        return MetricValue<Ratio>::available(Ratio{});
    }
    if (total < minimum_tcp_segments_for_retransmit_fraction) {
        return MetricValue<Ratio>::unavailable(MetricStatus::temporarily_unavailable);
    }
    return MetricValue<Ratio>::available(
        Ratio{static_cast<double>(retransmitted.value) / static_cast<double>(total)});
}

MetricValue<Ratio>
normalize_stall_fraction(const MetricValue<std::uint64_t>& previous_microseconds,
                         const MetricValue<std::uint64_t>& current_microseconds,
                         const std::chrono::steady_clock::duration elapsed) noexcept {
    if (!current_microseconds.has_value() || !previous_microseconds.has_value()) {
        return current_status_or_temporary<std::uint64_t, Ratio>(previous_microseconds,
                                                                 current_microseconds);
    }
    if (!valid_elapsed(elapsed) || current_microseconds.value < previous_microseconds.value) {
        return MetricValue<Ratio>::unavailable(MetricStatus::temporarily_unavailable);
    }
    const std::chrono::duration<double, std::micro> elapsed_microseconds{elapsed};
    const auto delta = current_microseconds.value - previous_microseconds.value;
    const auto fraction = static_cast<double>(delta) / elapsed_microseconds.count();
    constexpr double tolerance = 1.0e-6;
    if (fraction < 0.0 || fraction > 1.0 + tolerance) {
        return MetricValue<Ratio>::unavailable(MetricStatus::temporarily_unavailable);
    }
    return MetricValue<Ratio>::available(Ratio{std::min(fraction, 1.0)});
}

MetricValue<ByteCount> normalize_memory_used(const MetricValue<ByteCount>& total,
                                             const MetricValue<ByteCount>& available) noexcept {
    const auto status = combined_current_status(total.status, available.status);
    if (status != MetricStatus::available) {
        return MetricValue<ByteCount>::unavailable(status);
    }
    if (available.value > total.value) {
        return MetricValue<ByteCount>::unavailable(MetricStatus::temporarily_unavailable);
    }
    return MetricValue<ByteCount>::available(ByteCount{total.value.value - available.value.value});
}

MetricValue<Ratio> normalize_memory_usage(const MetricValue<ByteCount>& total,
                                          const MetricValue<ByteCount>& available) noexcept {
    const auto used = normalize_memory_used(total, available);
    if (!used.has_value()) {
        return MetricValue<Ratio>::unavailable(used.status);
    }
    if (total.value.value == 0U) {
        return MetricValue<Ratio>::unavailable(MetricStatus::temporarily_unavailable);
    }
    return MetricValue<Ratio>::available(
        Ratio{static_cast<double>(used.value.value) / static_cast<double>(total.value.value)});
}

SystemSample SystemTelemetryNormalizer::normalize(const RawTelemetrySnapshot& raw) noexcept {
    SystemSample result{};
    result.observed_at = raw.observed_at;
    result.memory_total = raw.system.memory_total;
    result.memory_used =
        normalize_memory_used(raw.system.memory_total, raw.system.memory_available);
    result.memory_usage =
        normalize_memory_usage(raw.system.memory_total, raw.system.memory_available);
    result.disk_read_latency = raw.system.disk_quality.read_latency;
    result.disk_write_latency = raw.system.disk_quality.write_latency;
    result.disk_service_time = raw.system.disk_quality.service_time;
    result.disk_queue_depth = raw.system.disk_quality.queue_depth;
    result.disk_service_concurrency = raw.system.disk_quality.service_concurrency;
    result.disk_worst_device_id = raw.system.disk_quality.worst_device_id;
    result.compressed_memory = raw.system.memory_activity.compressed_memory;
    result.network_connectivity = raw.system.network_quality.connectivity;
    result.network_active_interfaces = raw.system.network_quality.active_interfaces;
    result.gpu_usage = raw.system.gpu_usage;
    result.gpu_dedicated_memory = raw.system.gpu_dedicated_memory;
    result.gpu_shared_memory = raw.system.gpu_shared_memory;
    result.foreground_process = raw.system.foreground_process;
    result.foreground_application = raw.system.foreground_application;
    result.foreground_gpu_usage = raw.system.foreground_gpu_usage;
    result.dpc_usage = raw.system.dpc_usage;
    result.interrupt_usage = raw.system.interrupt_usage;
    result.dpc_rate = raw.system.dpc_rate;
    result.cpu_current_mhz = raw.system.cpu_current_mhz;
    result.cpu_max_mhz = raw.system.cpu_max_mhz;
    result.cpu_thermal_limit_mhz = raw.system.cpu_thermal_limit_mhz;
    result.cpu_thermal_limit_fraction = raw.system.cpu_thermal_limit_fraction;
    result.power_source = raw.system.power_source;
    result.battery_fraction = raw.system.battery_fraction;
    result.battery_saver = raw.system.battery_saver;
    result.system_uptime = raw.system.system_uptime;
    result.thermal_pressure_state = raw.system.thermal_pressure_state;
    result.memory_pressure_state = raw.system.memory_pressure_state;
    result.scheduler_delay = raw.system.scheduler_delay;
    result.logical_processor_count = raw.system.logical_processor_count;
    result.physical_processor_count = raw.system.physical_processor_count;
    result.active_processor_count = raw.system.active_processor_count;

    if (!previous_) {
        result.cpu_usage = initial_cumulative_status<CpuTimeCounters, Ratio>(raw.system.cpu_time);
        result.disk_read_rate =
            initial_cumulative_status<ByteCount, BytesPerSecond>(raw.system.disk_read_bytes);
        result.disk_write_rate =
            initial_cumulative_status<ByteCount, BytesPerSecond>(raw.system.disk_write_bytes);
        result.network_receive_rate =
            initial_cumulative_status<ByteCount, BytesPerSecond>(raw.system.network_receive_bytes);
        result.network_transmit_rate =
            initial_cumulative_status<ByteCount, BytesPerSecond>(raw.system.network_transmit_bytes);
        result.network_interface_changes = initial_cumulative_status<std::uint64_t, std::uint64_t>(
            raw.system.network_quality.interface_change_counter);
        result.network_tcp_retransmit_fraction = MetricValue<Ratio>::unavailable(
            raw.system.network_quality.tcp_out_segments.has_value() &&
                    raw.system.network_quality.tcp_retransmitted_segments.has_value()
                ? MetricStatus::temporarily_unavailable
            : !raw.system.network_quality.tcp_out_segments.has_value()
                ? raw.system.network_quality.tcp_out_segments.status
                : raw.system.network_quality.tcp_retransmitted_segments.status);
        result.network_tcp_failed_connections =
            initial_cumulative_status<std::uint64_t, std::uint64_t>(
                raw.system.network_quality.tcp_failed_connections);
        result.network_tcp_resets = initial_cumulative_status<std::uint64_t, std::uint64_t>(
            raw.system.network_quality.tcp_established_resets);
        result.memory_page_out_rate = initial_cumulative_status<ByteCount, BytesPerSecond>(
            raw.system.memory_activity.page_out_bytes);
        result.memory_swap_in_rate = initial_cumulative_status<ByteCount, BytesPerSecond>(
            raw.system.memory_activity.swap_in_bytes);
        result.memory_swap_out_rate = initial_cumulative_status<ByteCount, BytesPerSecond>(
            raw.system.memory_activity.swap_out_bytes);
        result.memory_compression_rate = initial_cumulative_status<ByteCount, BytesPerSecond>(
            raw.system.memory_activity.compressed_bytes);
        result.memory_decompression_rate = initial_cumulative_status<ByteCount, BytesPerSecond>(
            raw.system.memory_activity.decompressed_bytes);
        result.cpu_some_pressure = initial_cumulative_status<std::uint64_t, Ratio>(
            raw.system.pressure.cpu_some_microseconds);
        result.memory_some_pressure = initial_cumulative_status<std::uint64_t, Ratio>(
            raw.system.pressure.memory_some_microseconds);
        result.memory_full_pressure = initial_cumulative_status<std::uint64_t, Ratio>(
            raw.system.pressure.memory_full_microseconds);
        result.io_some_pressure = initial_cumulative_status<std::uint64_t, Ratio>(
            raw.system.pressure.io_some_microseconds);
        result.io_full_pressure = initial_cumulative_status<std::uint64_t, Ratio>(
            raw.system.pressure.io_full_microseconds);
        previous_ = PreviousObservation{raw.observed_at, raw.system};
        return result;
    }

    const auto elapsed = raw.observed_at - previous_->observed_at;
    result.cpu_usage =
        normalize_cpu_usage(previous_->system.cpu_time, raw.system.cpu_time, elapsed);
    result.disk_read_rate =
        normalize_byte_rate(previous_->system.disk_read_bytes, raw.system.disk_read_bytes, elapsed);
    result.disk_write_rate = normalize_byte_rate(previous_->system.disk_write_bytes,
                                                 raw.system.disk_write_bytes, elapsed);
    result.network_receive_rate = normalize_byte_rate(previous_->system.network_receive_bytes,
                                                      raw.system.network_receive_bytes, elapsed);
    result.network_transmit_rate = normalize_byte_rate(previous_->system.network_transmit_bytes,
                                                       raw.system.network_transmit_bytes, elapsed);
    result.network_interface_changes =
        normalize_counter_delta(previous_->system.network_quality.interface_change_counter,
                                raw.system.network_quality.interface_change_counter);
    result.network_tcp_retransmit_fraction = normalize_tcp_retransmit_fraction(
        previous_->system.network_quality.tcp_out_segments,
        raw.system.network_quality.tcp_out_segments,
        previous_->system.network_quality.tcp_retransmitted_segments,
        raw.system.network_quality.tcp_retransmitted_segments);
    result.network_tcp_failed_connections =
        normalize_counter_delta(previous_->system.network_quality.tcp_failed_connections,
                                raw.system.network_quality.tcp_failed_connections);
    result.network_tcp_resets =
        normalize_counter_delta(previous_->system.network_quality.tcp_established_resets,
                                raw.system.network_quality.tcp_established_resets);
    result.memory_page_out_rate = normalize_byte_rate(
        previous_->system.memory_activity.page_out_bytes,
        raw.system.memory_activity.page_out_bytes, elapsed);
    result.memory_swap_in_rate = normalize_byte_rate(
        previous_->system.memory_activity.swap_in_bytes,
        raw.system.memory_activity.swap_in_bytes, elapsed);
    result.memory_swap_out_rate = normalize_byte_rate(
        previous_->system.memory_activity.swap_out_bytes,
        raw.system.memory_activity.swap_out_bytes, elapsed);
    result.memory_compression_rate = normalize_byte_rate(
        previous_->system.memory_activity.compressed_bytes,
        raw.system.memory_activity.compressed_bytes, elapsed);
    result.memory_decompression_rate = normalize_byte_rate(
        previous_->system.memory_activity.decompressed_bytes,
        raw.system.memory_activity.decompressed_bytes, elapsed);
    result.cpu_some_pressure =
        normalize_stall_fraction(previous_->system.pressure.cpu_some_microseconds,
                                 raw.system.pressure.cpu_some_microseconds, elapsed);
    result.memory_some_pressure =
        normalize_stall_fraction(previous_->system.pressure.memory_some_microseconds,
                                 raw.system.pressure.memory_some_microseconds, elapsed);
    result.memory_full_pressure =
        normalize_stall_fraction(previous_->system.pressure.memory_full_microseconds,
                                 raw.system.pressure.memory_full_microseconds, elapsed);
    result.io_some_pressure =
        normalize_stall_fraction(previous_->system.pressure.io_some_microseconds,
                                 raw.system.pressure.io_some_microseconds, elapsed);
    result.io_full_pressure =
        normalize_stall_fraction(previous_->system.pressure.io_full_microseconds,
                                 raw.system.pressure.io_full_microseconds, elapsed);

    // A non-monotonic sample is observable but cannot become the next delta
    // baseline.
    if (valid_elapsed(elapsed)) {
        previous_ = PreviousObservation{raw.observed_at, raw.system};
    }
    return result;
}

void SystemTelemetryNormalizer::reset() noexcept { previous_.reset(); }

} // namespace blackbox::telemetry
