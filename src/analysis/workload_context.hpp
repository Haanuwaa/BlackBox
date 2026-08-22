#pragma once

#include "analysis/incident_analyzer.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>

namespace blackbox::analysis {

inline constexpr std::size_t maximum_context_process_metadata = 512U;
inline constexpr std::size_t maximum_context_evidence = 8U;

struct WorkloadContextConfiguration {
    bool enabled{true};
    std::size_t maximum_process_metadata{maximum_context_process_metadata};
    std::size_t maximum_evidence{maximum_context_evidence};
    double maximum_score_reduction{0.20};
    friend constexpr bool operator==(const WorkloadContextConfiguration&,
                                     const WorkloadContextConfiguration&) = default;
};

enum class WorkloadContextConfigurationError : std::uint8_t {
    process_metadata_limit_invalid,
    evidence_limit_invalid,
    score_reduction_invalid,
};

[[nodiscard]] std::expected<WorkloadContextConfiguration,
                            WorkloadContextConfigurationError>
validate_workload_context_configuration(
    WorkloadContextConfiguration configuration) noexcept;

[[nodiscard]] WorkloadContextAssessment recognize_workload_context(
    const core::IncidentSnapshot& incident,
    WorkloadContextConfiguration configuration = {});

// Keeps raw statistical/personalized evidence unchanged. Only public ranking scores are
// softly reduced according to the complete probability distribution, never a hard label.
void apply_workload_context(IncidentAnalysis& analysis,
                            const WorkloadContextConfiguration& configuration) noexcept;

} // namespace blackbox::analysis
