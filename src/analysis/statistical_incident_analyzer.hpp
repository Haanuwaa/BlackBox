#pragma once

#include "analysis/incident_analyzer.hpp"
#include "analysis/workload_context.hpp"

#include <chrono>
#include <cstddef>
#include <expected>

namespace blackbox::analysis {

struct ResourceEffectFloorConfiguration {
    double cpu_minimum_value{0.65};
    double cpu_minimum_increase{0.15};
    double memory_minimum_value{0.80};
    double memory_minimum_increase{0.05};
    double disk_throughput_minimum_bytes_per_second{8.0 * 1024.0 * 1024.0};
    double disk_throughput_minimum_increase{4.0 * 1024.0 * 1024.0};
    double disk_latency_minimum_seconds{0.020};
    double disk_latency_minimum_increase_seconds{0.010};
    double disk_queue_minimum_depth{2.0};
    double disk_queue_minimum_increase{1.0};
    double network_throughput_minimum_bytes_per_second{8.0 * 1024.0 * 1024.0};
    double network_throughput_minimum_increase{4.0 * 1024.0 * 1024.0};
    double network_retransmit_minimum_fraction{0.05};
    double network_retransmit_minimum_increase{0.02};
    double network_quality_counter_minimum{1.0};
    double network_connectivity_minimum_severity{2.0};
    friend constexpr bool operator==(const ResourceEffectFloorConfiguration&,
                                     const ResourceEffectFloorConfiguration&) = default;
};

struct StatisticalAnalysisConfiguration {
    std::chrono::nanoseconds baseline_duration{std::chrono::seconds{60}};
    std::chrono::nanoseconds evaluation_pre_window{std::chrono::seconds{30}};
    std::size_t baseline_capacity{256U};
    std::size_t minimum_baseline_samples{8U};
    std::size_t maximum_process_candidates{512U};
    std::size_t maximum_ranked_processes{100U};
    ResourceEffectFloorConfiguration resource_effect_floors{};
    WorkloadContextConfiguration workload_context{};
    friend constexpr bool operator==(const StatisticalAnalysisConfiguration&,
                                     const StatisticalAnalysisConfiguration&) = default;
};

enum class StatisticalAnalysisConfigurationError : std::uint8_t {
    baseline_duration_not_positive,
    evaluation_pre_window_negative,
    baseline_capacity_zero,
    minimum_baseline_samples_invalid,
    process_candidate_limit_zero,
    ranked_process_limit_invalid,
    resource_effect_floor_invalid,
    workload_context_invalid,
};

[[nodiscard]] std::expected<StatisticalAnalysisConfiguration,
                            StatisticalAnalysisConfigurationError>
validate_statistical_analysis_configuration(
    StatisticalAnalysisConfiguration configuration) noexcept;

class StatisticalIncidentAnalyzer final : public IIncidentAnalyzer {
public:
    explicit StatisticalIncidentAnalyzer(
        StatisticalAnalysisConfiguration configuration = {});

    [[nodiscard]] std::expected<IncidentAnalysis, AnalysisError> analyze(
        const core::IncidentSnapshot& incident) const noexcept override;
    [[nodiscard]] const StatisticalAnalysisConfiguration& configuration() const noexcept;

private:
    [[nodiscard]] IncidentAnalysis analyze_checked(
        const core::IncidentSnapshot& incident) const;

    StatisticalAnalysisConfiguration configuration_{};
};

} // namespace blackbox::analysis
