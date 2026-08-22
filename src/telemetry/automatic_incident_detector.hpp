#pragma once

#include "core/incident.hpp"
#include "telemetry/types.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

namespace blackbox::telemetry {

struct DetectorMetricConfiguration {
    double hard_threshold{};
    double statistical_floor{};
    double minimum_standard_deviation{};
    friend constexpr bool operator==(const DetectorMetricConfiguration&,
                                     const DetectorMetricConfiguration&) = default;
};

struct AutomaticDetectorConfiguration {
    DetectorMetricConfiguration cpu{0.98, 0.85, 0.02};
    DetectorMetricConfiguration memory{0.97, 0.90, 0.01};
    DetectorMetricConfiguration disk{1024.0 * 1024.0 * 1024.0,
                                     128.0 * 1024.0 * 1024.0,
                                     8.0 * 1024.0 * 1024.0};
    DetectorMetricConfiguration network{512.0 * 1024.0 * 1024.0,
                                        64.0 * 1024.0 * 1024.0,
                                        4.0 * 1024.0 * 1024.0};
    double disk_stall_seconds{0.100};
    double disk_queue_depth{8.0};
    double network_retransmit_fraction{0.25};
    std::uint64_t network_failure_events{2U};
    std::size_t baseline_samples{30U};
    std::uint32_t consecutive_samples{3U};
    double statistical_z_score{8.0};
    std::chrono::nanoseconds cooldown{std::chrono::minutes{2}};
    bool cpu_enabled{true};
    bool memory_enabled{true};
    bool disk_enabled{true};
    bool network_enabled{true};
    friend constexpr bool operator==(const AutomaticDetectorConfiguration&,
                                     const AutomaticDetectorConfiguration&) = default;
};

enum class AutomaticDetectorConfigurationError : std::uint8_t {
    invalid_metric_threshold,
    invalid_baseline_samples,
    invalid_consecutive_samples,
    invalid_z_score,
    negative_cooldown,
    invalid_event_threshold,
};

struct AutomaticDetectorDiagnostics {
    std::uint64_t samples_observed{};
    std::uint64_t triggers_emitted{};
    std::uint64_t triggers_suppressed_by_cooldown{};
    std::uint64_t unavailable_metric_values{};
    std::uint64_t single_observation_triggers{};
    friend constexpr bool operator==(const AutomaticDetectorDiagnostics&,
                                     const AutomaticDetectorDiagnostics&) = default;
};

[[nodiscard]] std::expected<AutomaticDetectorConfiguration,
                            AutomaticDetectorConfigurationError>
validate_automatic_detector_configuration(AutomaticDetectorConfiguration configuration) noexcept;

class IAutomaticIncidentDetector {
public:
    virtual ~IAutomaticIncidentDetector() = default;
    [[nodiscard]] virtual std::optional<core::IncidentCaptureTrigger> observe(
        const SystemSample& sample) noexcept = 0;
    virtual void reset() noexcept = 0;
    [[nodiscard]] virtual AutomaticDetectorDiagnostics diagnostics() const noexcept = 0;
};

// Fixed storage and constant work keep this safe on the collector path. A
// detector instance owns four independent 60-sample rolling baselines and
// emits at most one trigger for an observation.
class AutomaticIncidentDetector final : public IAutomaticIncidentDetector {
public:
    explicit AutomaticIncidentDetector(AutomaticDetectorConfiguration configuration = {});

    [[nodiscard]] std::optional<core::IncidentCaptureTrigger> observe(
        const SystemSample& sample) noexcept override;
    void reset() noexcept override;
    [[nodiscard]] std::expected<void, AutomaticDetectorConfigurationError>
    reconfigure(AutomaticDetectorConfiguration configuration) noexcept;
    [[nodiscard]] AutomaticDetectorDiagnostics diagnostics() const noexcept override;
    [[nodiscard]] const AutomaticDetectorConfiguration& configuration() const noexcept;

private:
    static constexpr std::size_t maximum_baseline_samples = 60U;
    struct MetricState {
        std::array<double, maximum_baseline_samples> values{};
        double sum{};
        double sum_squares{};
        std::size_t size{};
        std::size_t next{};
        std::uint32_t consecutive{};
    };

    struct Candidate {
        core::AutomaticIncidentResource resource{core::AutomaticIncidentResource::none};
        double observed{};
        double baseline{};
        double score{};
        bool ready{};
        core::AutomaticIncidentSignal signal{
            core::AutomaticIncidentSignal::throughput_or_utilization};
        bool single_observation{};
    };

    [[nodiscard]] Candidate evaluate(core::AutomaticIncidentResource resource,
                                     double value,
                                     const DetectorMetricConfiguration& configuration,
                                     MetricState& state) noexcept;
    void observe_unavailable(MetricState& state) noexcept;

    AutomaticDetectorConfiguration configuration_{};
    std::array<MetricState, 4U> metrics_{};
    AutomaticDetectorDiagnostics diagnostics_{};
    std::optional<core::MonotonicTimePoint> last_trigger_at_{};
};

} // namespace blackbox::telemetry
