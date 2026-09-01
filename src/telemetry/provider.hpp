#pragma once

#include "telemetry/types.hpp"

#include <cstdint>

namespace blackbox::telemetry {

struct PlatformCapabilities {
    bool cpu_usage{};
    bool memory_usage{};
    bool process_cpu{};
    bool process_memory{};
    bool process_disk_io{};
    bool disk_throughput{};
    bool disk_latency{};
    bool disk_queue_depth{};
    bool disk_service_time{};
    bool network_usage{};
    bool network_connectivity{};
    bool network_transport_quality{};
    bool per_process_network{};
    bool gpu_usage{};
    bool gpu_memory{};
    bool gpu_inventory{};
    bool foreground_application{};
    bool foreground_gpu_usage{};
    bool dpc_isr{};
    bool cpu_frequency{};
    bool cpu_thermal_limit{};
    bool power_status{};
    bool system_uptime{};
    bool cpu_some_pressure{};
    bool memory_some_pressure{};
    bool memory_full_pressure{};
    bool io_some_pressure{};
    bool io_full_pressure{};
    bool thermal_pressure_state{};
    friend constexpr bool operator==(const PlatformCapabilities&,
                                     const PlatformCapabilities&) = default;
};

// Slow-changing, non-identifying device evidence. Providers must not expose
// bus addresses, serial numbers, device names, or process identifiers here.
// This is deliberately separate from the recorded time series: it describes
// collection/render capability, not incident causality.
struct GpuInventoryEvidence {
    MetricValue<std::uint32_t> device_count{};
    MetricValue<std::uint32_t> integrated_device_count{};
    MetricValue<std::uint32_t> discrete_device_count{};
    MetricValue<std::uint32_t> unknown_device_count{};
    MetricValue<bool> render_device_available{};
    friend constexpr bool operator==(const GpuInventoryEvidence&,
                                     const GpuInventoryEvidence&) = default;
};

enum class ProviderSampleStatus : std::uint8_t {
    complete,
    partial,
    temporarily_failed,
};

enum class ProviderContractViolation : std::uint8_t {
    none,
    sampled_tiers_mismatch,
    capability_status_mismatch,
    invalid_cpu_counters,
    invalid_memory_counters,
    invalid_disk_quality,
    invalid_network_quality,
    invalid_gpu_evidence,
    invalid_responsiveness_evidence,
    invalid_power_evidence,
    invalid_pressure_evidence,
    invalid_process_identity,
    duplicate_process_identity,
    invalid_gpu_inventory,
};

struct ProviderSampleResult {
    ProviderSampleStatus status{ProviderSampleStatus::complete};
    std::uint64_t sequence{};
    friend constexpr bool operator==(const ProviderSampleResult&,
                                     const ProviderSampleResult&) = default;
};

class ITelemetryProvider {
public:
    virtual ~ITelemetryProvider() = default;

    // Called once on the collector worker before its first deadline. Platform
    // providers may apply bounded scheduling policy to that current thread.
    [[nodiscard]] virtual bool prepare_sampling_thread() noexcept { return true; }

    // The caller owns and reuses destination. Providers clear logical contents
    // while retaining capacity, avoiding a forced allocation policy.
    [[nodiscard]] virtual ProviderSampleResult sample(SamplingRequest request,
                                                      RawTelemetrySnapshot& destination) = 0;

    [[nodiscard]] virtual PlatformCapabilities capabilities() const noexcept = 0;

    [[nodiscard]] virtual GpuInventoryEvidence gpu_inventory() const noexcept { return {}; }
};

// Backend-independent conformance check used by provider tests and bring-up
// tools. It performs no OS calls and is intentionally not inserted into the
// collection hot path.
[[nodiscard]] ProviderContractViolation
validate_provider_snapshot_contract(const PlatformCapabilities& capabilities,
                                    SamplingRequest request,
                                    const RawTelemetrySnapshot& snapshot) noexcept;

[[nodiscard]] ProviderContractViolation
validate_gpu_inventory_contract(const PlatformCapabilities& capabilities,
                                const GpuInventoryEvidence& inventory) noexcept;

} // namespace blackbox::telemetry
