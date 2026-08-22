#pragma once

#include "analysis/incident_analyzer.hpp"

#include <cstddef>
#include <vector>

namespace blackbox::analysis {

inline constexpr std::size_t maximum_contributor_candidates = 20U;

// Scores substantial absolute process activity when an incident begins before a
// per-process baseline can be established. This is deliberately only an activity
// score: callers must retain low confidence and must not present it as causation.
[[nodiscard]] double cold_start_process_activity_score(
    const MetricAnomalyEvidence& evidence) noexcept;

// Ranks process activity using correlation evidence only. The result deliberately has no
// "proven cause" state and separates activity beginning before the marker from later reactions.
[[nodiscard]] std::vector<ContributorCandidate> rank_contributors(
    const core::IncidentSnapshot& incident,
    const IncidentAnalysis& analysis,
    const IncidentAnalysisContext& context = {},
    std::size_t maximum_results = maximum_contributor_candidates);

} // namespace blackbox::analysis
