#pragma once

#include "app/incident_viewer_service.hpp"

#if BLACKBOX_ANALYSIS_ENABLED
#include "analysis/incident_analyzer.hpp"
#include "analysis/incident_clustering.hpp"

#include <cstdint>
#include <string>

namespace blackbox::app::detail {

[[nodiscard]] ui::IncidentAnalysisView
analyze_incident(analysis::IIncidentAnalyzer* analyzer,
                 storage::IProcessProfileRepository* profile_repository,
                 storage::IFeedbackCalibrationRepository* feedback_repository,
                 std::int64_t incident_id, const core::IncidentSnapshot& incident,
                 const ui::RecurringIncidentView& recurring);

[[nodiscard]] std::string
shared_characteristics_text(const analysis::IncidentCluster& cluster);

} // namespace blackbox::app::detail
#endif
