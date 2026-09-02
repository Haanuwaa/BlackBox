#pragma once

#include "core/clock.hpp"

#include <chrono>
#include <compare>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace blackbox::telemetry {

enum class MetricStatus : std::uint8_t {
    available,
    unsupported,
    inaccessible,
    temporarily_unavailable,
};

template <typename T> struct MetricValue {
    T value{};
    MetricStatus status{MetricStatus::unsupported};

    [[nodiscard]] static constexpr MetricValue available(T new_value) {
        return MetricValue{std::move(new_value), MetricStatus::available};
    }

    [[nodiscard]] static constexpr MetricValue unavailable(const MetricStatus reason) {
        return MetricValue{T{}, reason};
    }

    [[nodiscard]] constexpr bool has_value() const noexcept {
        return status == MetricStatus::available;
    }

    friend constexpr bool operator==(const MetricValue&, const MetricValue&) = default;
};

struct Ratio {
    double value{};
    friend constexpr bool operator==(const Ratio&, const Ratio&) = default;
};

struct ByteCount {
    std::uint64_t value{};
    friend constexpr auto operator<=>(const ByteCount&, const ByteCount&) = default;
};

struct BytesPerSecond {
    double value{};
    friend constexpr bool operator==(const BytesPerSecond&, const BytesPerSecond&) = default;
};

struct Seconds {
    double value{};
    friend constexpr bool operator==(const Seconds&, const Seconds&) = default;
};

enum class NetworkConnectivityLevel : std::uint8_t {
    disconnected,
    local,
    internet,
    constrained,
    unknown,
};

enum class PowerSource : std::uint8_t {
    ac,
    battery,
    ups_or_short_term,
    unknown,
};

enum class ThermalPressureState : std::uint8_t {
    nominal,
    fair,
    serious,
    critical,
    unknown,
};

// Coarse event-latched system memory pressure. This is intentionally not a
// Linux PSI fraction: it carries no cumulative stalled-time meaning.
enum class MemoryPressureState : std::uint8_t {
    normal,
    warning,
    critical,
    unknown,
};

// Cumulative machine-wide stalled durations. Linux PSI supplies microseconds;
// other providers leave these dimensions unsupported unless they can prove
// the same cumulative wall-time semantics.
struct RawPressureCounters {
    MetricValue<std::uint64_t> cpu_some_microseconds{};
    MetricValue<std::uint64_t> memory_some_microseconds{};
    MetricValue<std::uint64_t> memory_full_microseconds{};
    MetricValue<std::uint64_t> io_some_microseconds{};
    MetricValue<std::uint64_t> io_full_microseconds{};
    friend constexpr bool operator==(const RawPressureCounters&,
                                     const RawPressureCounters&) = default;
};

// Disk quality values describe the physical-disk layer observed over the
// provider interval. They are deliberately separate from process I/O bytes,
// which do not imply device latency or queueing.
struct RawDiskQuality {
    MetricValue<Seconds> read_latency{};
    MetricValue<Seconds> write_latency{};
    MetricValue<Seconds> service_time{};
    MetricValue<double> queue_depth{};
    MetricValue<std::uint64_t> worst_device_id{};
    friend constexpr bool operator==(const RawDiskQuality&, const RawDiskQuality&) = default;
};

// These host-wide TCP counters are passive transport evidence. Retransmission
// is not labeled as physical packet loss, and failed/reset connections do not
// identify an application or remote endpoint.
struct RawNetworkQuality {
    MetricValue<NetworkConnectivityLevel> connectivity{};
    MetricValue<std::uint64_t> active_interfaces{};
    MetricValue<std::uint64_t> interface_change_counter{};
    MetricValue<std::uint64_t> tcp_out_segments{};
    MetricValue<std::uint64_t> tcp_retransmitted_segments{};
    MetricValue<std::uint64_t> tcp_failed_connections{};
    MetricValue<std::uint64_t> tcp_established_resets{};
    friend constexpr bool operator==(const RawNetworkQuality&, const RawNetworkQuality&) = default;
};

struct CpuTimeCounters {
    std::uint64_t busy_ticks{};
    std::uint64_t total_ticks{};
    friend constexpr bool operator==(const CpuTimeCounters&, const CpuTimeCounters&) = default;
};

struct ProcessId {
    std::uint32_t value{};
    friend constexpr auto operator<=>(const ProcessId&, const ProcessId&) = default;
};

struct ProcessIdentity {
    ProcessId pid{};
    std::uint64_t creation_token{};
    friend constexpr auto operator<=>(const ProcessIdentity&, const ProcessIdentity&) = default;
};

// Privacy-reduced application identity for native surfaces that can identify
// an active application but cannot prove a PID/creation-token pair. Both
// tokens are session-scoped and opaque; neither is a native application ID.
struct OpaqueApplicationIdentity {
    std::uint64_t session_token{};
    std::uint64_t application_token{};
    friend constexpr auto operator<=>(const OpaqueApplicationIdentity&,
                                      const OpaqueApplicationIdentity&) = default;
};

struct ProcessInfo {
    ProcessIdentity identity{};
    MetricValue<ProcessId> parent_pid{};
    MetricValue<std::string> name{};
    MetricValue<std::string> executable_path{};
    friend bool operator==(const ProcessInfo&, const ProcessInfo&) = default;
};

struct RawProcessCounters {
    ProcessIdentity identity{};
    MetricValue<std::chrono::nanoseconds> cpu_time{};
    MetricValue<ByteCount> working_set{};
    MetricValue<ByteCount> disk_read_bytes{};
    MetricValue<ByteCount> disk_write_bytes{};
    friend constexpr bool operator==(const RawProcessCounters&,
                                     const RawProcessCounters&) = default;
};

struct ProcessSample {
    ProcessIdentity identity{};
    MetricValue<Ratio> cpu_usage{};
    MetricValue<ByteCount> working_set{};
    MetricValue<BytesPerSecond> disk_read_rate{};
    MetricValue<BytesPerSecond> disk_write_rate{};
    friend constexpr bool operator==(const ProcessSample&, const ProcessSample&) = default;
};

struct ProcessFrame {
    core::MonotonicTimePoint observed_at{};
    std::vector<ProcessSample> processes{};
    friend bool operator==(const ProcessFrame&, const ProcessFrame&) = default;
};

enum class RawProcessLifecycleKind : std::uint8_t {
    started,
    exited,
};

struct RawProcessLifecycleEvent {
    ProcessIdentity identity{};
    RawProcessLifecycleKind kind{RawProcessLifecycleKind::started};
    friend constexpr bool operator==(const RawProcessLifecycleEvent&,
                                     const RawProcessLifecycleEvent&) = default;
};

struct RawProcessCollectionDiagnostics {
    std::uint32_t enumerated{};
    std::uint32_t sampled{};
    std::uint32_t inaccessible{};
    std::uint32_t exited_during_sample{};
    std::uint32_t metadata_resolved{};
    std::uint32_t metadata_failures{};
    friend constexpr bool operator==(const RawProcessCollectionDiagnostics&,
                                     const RawProcessCollectionDiagnostics&) = default;
};

struct RawSystemCounters {
    MetricValue<CpuTimeCounters> cpu_time{};
    MetricValue<ByteCount> memory_total{};
    MetricValue<ByteCount> memory_available{};
    MetricValue<ByteCount> disk_read_bytes{};
    MetricValue<ByteCount> disk_write_bytes{};
    MetricValue<ByteCount> network_receive_bytes{};
    MetricValue<ByteCount> network_transmit_bytes{};
    RawDiskQuality disk_quality{};
    RawNetworkQuality network_quality{};
    MetricValue<Ratio> gpu_usage{};
    MetricValue<ByteCount> gpu_dedicated_memory{};
    MetricValue<ByteCount> gpu_shared_memory{};
    MetricValue<ProcessIdentity> foreground_process{};
    MetricValue<OpaqueApplicationIdentity> foreground_application{};
    MetricValue<Ratio> foreground_gpu_usage{};
    MetricValue<Ratio> dpc_usage{};
    MetricValue<Ratio> interrupt_usage{};
    MetricValue<double> dpc_rate{};
    MetricValue<double> cpu_current_mhz{};
    MetricValue<double> cpu_max_mhz{};
    MetricValue<double> cpu_thermal_limit_mhz{};
    MetricValue<Ratio> cpu_thermal_limit_fraction{};
    MetricValue<PowerSource> power_source{};
    MetricValue<Ratio> battery_fraction{};
    MetricValue<bool> battery_saver{};
    MetricValue<Seconds> system_uptime{};
    RawPressureCounters pressure{};
    MetricValue<ThermalPressureState> thermal_pressure_state{};
    MetricValue<MemoryPressureState> memory_pressure_state{};
    MetricValue<std::uint32_t> logical_processor_count{};
    friend constexpr bool operator==(const RawSystemCounters&, const RawSystemCounters&) = default;
};

struct SystemSample {
    core::MonotonicTimePoint observed_at{};
    MetricValue<Ratio> cpu_usage{};
    MetricValue<ByteCount> memory_used{};
    MetricValue<ByteCount> memory_total{};
    MetricValue<Ratio> memory_usage{};
    MetricValue<BytesPerSecond> disk_read_rate{};
    MetricValue<BytesPerSecond> disk_write_rate{};
    MetricValue<BytesPerSecond> network_receive_rate{};
    MetricValue<BytesPerSecond> network_transmit_rate{};
    MetricValue<Seconds> disk_read_latency{};
    MetricValue<Seconds> disk_write_latency{};
    MetricValue<Seconds> disk_service_time{};
    MetricValue<double> disk_queue_depth{};
    MetricValue<std::uint64_t> disk_worst_device_id{};
    MetricValue<NetworkConnectivityLevel> network_connectivity{};
    MetricValue<std::uint64_t> network_active_interfaces{};
    MetricValue<std::uint64_t> network_interface_changes{};
    MetricValue<Ratio> network_tcp_retransmit_fraction{};
    MetricValue<std::uint64_t> network_tcp_failed_connections{};
    MetricValue<std::uint64_t> network_tcp_resets{};
    MetricValue<Ratio> gpu_usage{};
    MetricValue<ByteCount> gpu_dedicated_memory{};
    MetricValue<ByteCount> gpu_shared_memory{};
    MetricValue<ProcessIdentity> foreground_process{};
    MetricValue<OpaqueApplicationIdentity> foreground_application{};
    MetricValue<Ratio> foreground_gpu_usage{};
    MetricValue<Ratio> dpc_usage{};
    MetricValue<Ratio> interrupt_usage{};
    MetricValue<double> dpc_rate{};
    MetricValue<double> cpu_current_mhz{};
    MetricValue<double> cpu_max_mhz{};
    MetricValue<double> cpu_thermal_limit_mhz{};
    MetricValue<Ratio> cpu_thermal_limit_fraction{};
    MetricValue<PowerSource> power_source{};
    MetricValue<Ratio> battery_fraction{};
    MetricValue<bool> battery_saver{};
    MetricValue<Seconds> system_uptime{};
    MetricValue<Ratio> cpu_some_pressure{};
    MetricValue<Ratio> memory_some_pressure{};
    MetricValue<Ratio> memory_full_pressure{};
    MetricValue<Ratio> io_some_pressure{};
    MetricValue<Ratio> io_full_pressure{};
    MetricValue<ThermalPressureState> thermal_pressure_state{};
    MetricValue<MemoryPressureState> memory_pressure_state{};
    friend constexpr bool operator==(const SystemSample&, const SystemSample&) = default;
};

enum class SamplingTier : std::uint8_t {
    fast = 1U << 0U,
    normal = 1U << 1U,
    slow = 1U << 2U,
};

class SamplingTierSet {
public:
    constexpr SamplingTierSet() = default;
    constexpr SamplingTierSet(const SamplingTier tier) : bits_{static_cast<std::uint8_t>(tier)} {}

    [[nodiscard]] static constexpr SamplingTierSet all() noexcept {
        return SamplingTierSet{static_cast<std::uint8_t>(SamplingTier::fast) |
                               static_cast<std::uint8_t>(SamplingTier::normal) |
                               static_cast<std::uint8_t>(SamplingTier::slow)};
    }

    [[nodiscard]] constexpr bool contains(const SamplingTier tier) const noexcept {
        return (bits_ & static_cast<std::uint8_t>(tier)) != 0U;
    }

    friend constexpr SamplingTierSet operator|(const SamplingTierSet left,
                                               const SamplingTier right) noexcept {
        return SamplingTierSet{
            static_cast<std::uint8_t>(left.bits_ | static_cast<std::uint8_t>(right))};
    }

    friend constexpr bool operator==(const SamplingTierSet&, const SamplingTierSet&) = default;

private:
    explicit constexpr SamplingTierSet(const std::uint8_t bits) : bits_{bits} {}

    std::uint8_t bits_{};
};

[[nodiscard]] constexpr SamplingTierSet operator|(const SamplingTier left,
                                                  const SamplingTier right) noexcept {
    return SamplingTierSet{left} | right;
}

struct SamplingRequest {
    SamplingTierSet tiers{SamplingTierSet::all()};
    bool collect_foreground_application{true};
    friend constexpr bool operator==(const SamplingRequest&, const SamplingRequest&) = default;
};

struct RawTelemetrySnapshot {
    core::MonotonicTimePoint observed_at{};
    SamplingTierSet sampled_tiers{};
    RawSystemCounters system{};
    std::vector<RawProcessCounters> processes{};
    std::vector<ProcessInfo> process_metadata{};
    std::vector<RawProcessLifecycleEvent> process_lifecycle_events{};
    RawProcessCollectionDiagnostics process_diagnostics{};

    // Clearing preserves vector capacity so a caller can reuse this buffer.
    void reset(const core::MonotonicTimePoint time, const SamplingTierSet tiers) {
        observed_at = time;
        sampled_tiers = tiers;
        system = RawSystemCounters{};
        processes.clear();
        process_metadata.clear();
        process_lifecycle_events.clear();
        process_diagnostics = RawProcessCollectionDiagnostics{};
    }

    friend bool operator==(const RawTelemetrySnapshot&, const RawTelemetrySnapshot&) = default;
};

} // namespace blackbox::telemetry
