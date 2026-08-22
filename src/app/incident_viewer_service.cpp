#include "app/incident_viewer_service.hpp"

#if BLACKBOX_ANALYSIS_ENABLED
#include "analysis/incident_analyzer.hpp"
#include "analysis/incident_clustering.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <utility>

namespace blackbox::app {
namespace {

[[nodiscard]] storage::IncidentListSort storage_order(
    const ui::IncidentListOrder order) noexcept {
    switch (order) {
    case ui::IncidentListOrder::newest_first:
        return storage::IncidentListSort::newest_first;
    case ui::IncidentListOrder::oldest_first:
        return storage::IncidentListSort::oldest_first;
    case ui::IncidentListOrder::longest_first:
        return storage::IncidentListSort::longest_first;
    case ui::IncidentListOrder::shortest_first:
        return storage::IncidentListSort::shortest_first;
    case ui::IncidentListOrder::label_ascending:
        return storage::IncidentListSort::label_ascending;
    case ui::IncidentListOrder::label_descending:
        return storage::IncidentListSort::label_descending;
    }
    return storage::IncidentListSort::newest_first;
}

[[nodiscard]] double milliseconds(const std::chrono::steady_clock::duration value) noexcept {
    return std::chrono::duration<double, std::milli>{value}.count();
}

[[nodiscard]] std::string note_preview(const std::string& note) {
    constexpr std::size_t maximum = 96U;
    if (note.size() <= maximum) return note;
    return note.substr(0U, maximum - 3U) + "...";
}

#if BLACKBOX_ANALYSIS_ENABLED
[[nodiscard]] constexpr std::string_view resource_name(
    const analysis::ResourceKind resource) noexcept {
    switch (resource) {
    case analysis::ResourceKind::cpu: return "CPU";
    case analysis::ResourceKind::memory: return "Memory";
    case analysis::ResourceKind::disk: return "Disk";
    case analysis::ResourceKind::network: return "Network";
    }
    return "Unknown resource";
}

[[nodiscard]] constexpr std::string_view metric_name(
    const analysis::MetricKind metric) noexcept {
    switch (metric) {
    case analysis::MetricKind::system_cpu: return "CPU";
    case analysis::MetricKind::system_memory: return "memory";
    case analysis::MetricKind::disk_read: return "disk read";
    case analysis::MetricKind::disk_write: return "disk write";
    case analysis::MetricKind::disk_read_latency: return "physical-disk read latency";
    case analysis::MetricKind::disk_write_latency: return "physical-disk write latency";
    case analysis::MetricKind::disk_service_time: return "physical-disk service time";
    case analysis::MetricKind::disk_queue_depth: return "physical-disk queue depth";
    case analysis::MetricKind::network_receive: return "network receive";
    case analysis::MetricKind::network_transmit: return "network transmit";
    case analysis::MetricKind::network_connectivity:
        return "Windows connectivity disruption";
    case analysis::MetricKind::network_interface_changes: return "interface transitions";
    case analysis::MetricKind::network_tcp_retransmit: return "TCP retransmission fraction";
    case analysis::MetricKind::network_tcp_failures: return "failed TCP connections";
    case analysis::MetricKind::network_tcp_resets: return "reset TCP connections";
    case analysis::MetricKind::process_cpu: return "CPU";
    case analysis::MetricKind::process_working_set: return "working set";
    case analysis::MetricKind::process_disk_read: return "disk read";
    case analysis::MetricKind::process_disk_write: return "disk write";
    }
    return "metric";
}

[[nodiscard]] constexpr std::string_view confidence_name(
    const analysis::AnalysisConfidence confidence) noexcept {
    switch (confidence) {
    case analysis::AnalysisConfidence::unavailable: return "Unavailable";
    case analysis::AnalysisConfidence::low: return "Low / cold start";
    case analysis::AnalysisConfidence::moderate: return "Moderate";
    case analysis::AnalysisConfidence::high: return "High";
    }
    return "Unavailable";
}

[[nodiscard]] constexpr std::string_view context_name(
    const analysis::WorkloadContextKind context) noexcept {
    switch (context) {
    case analysis::WorkloadContextKind::unknown: return "Unknown";
    case analysis::WorkloadContextKind::idle: return "Idle";
    case analysis::WorkloadContextKind::gaming: return "Gaming";
    case analysis::WorkloadContextKind::development: return "Development";
    case analysis::WorkloadContextKind::compilation: return "Compilation";
    case analysis::WorkloadContextKind::video_playback_or_call:
        return "Video playback/call";
    case analysis::WorkloadContextKind::heavy_download: return "Heavy download";
    case analysis::WorkloadContextKind::desktop: return "Desktop";
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::string_view incident_type_name(
    const analysis::IncidentType type) noexcept {
    switch (type) {
    case analysis::IncidentType::unknown: return "Unknown";
    case analysis::IncidentType::cpu_pressure: return "CPU pressure pattern";
    case analysis::IncidentType::memory_pressure: return "Memory pressure pattern";
    case analysis::IncidentType::storage_pressure: return "Storage pressure pattern";
    case analysis::IncidentType::network_pressure: return "Network pressure pattern";
    case analysis::IncidentType::multi_resource_pressure:
        return "Multi-resource pressure pattern";
    case analysis::IncidentType::application_crash:
        return "Windows-reported application crash";
    case analysis::IncidentType::application_hang:
        return "Windows-reported application hang";
    case analysis::IncidentType::dns_resolution_timeout:
        return "Windows-reported DNS resolution timeout";
    case analysis::IncidentType::display_driver_recovery:
        return "Windows display timeout recovery";
    case analysis::IncidentType::storage_io_retry:
        return "Windows storage I/O retry";
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::string_view explanation_basis_name(
    const analysis::SymptomExplanationBasis basis) noexcept {
    switch (basis) {
    case analysis::SymptomExplanationBasis::none:
        return "No aligned symptom evidence";
    case analysis::SymptomExplanationBasis::automatic_capture_alignment:
        return "Aligned with the automatic capture signal";
    case analysis::SymptomExplanationBasis::resource_quality_signal:
        return "Direct resource-quality degradation signal";
    case analysis::SymptomExplanationBasis::preceding_contributor_alignment:
        return "Aligned preceding process activity";
    case analysis::SymptomExplanationBasis::system_event_alignment:
        return "Aligned Windows system event";
    }
    return "No aligned symptom evidence";
}

[[nodiscard]] constexpr analysis::SimilarIncidentSymptom similar_symptom(
    const ui::IncidentCategory category) noexcept {
    switch (category) {
    case ui::IncidentCategory::unknown:
        return analysis::SimilarIncidentSymptom::unknown;
    case ui::IncidentCategory::system_freeze:
        return analysis::SimilarIncidentSymptom::system_freeze;
    case ui::IncidentCategory::game_stutter:
        return analysis::SimilarIncidentSymptom::game_stutter;
    case ui::IncidentCategory::application_slowdown_or_hang:
        return analysis::SimilarIncidentSymptom::application_slowdown_or_hang;
    case ui::IncidentCategory::network:
        return analysis::SimilarIncidentSymptom::network;
    case ui::IncidentCategory::audio:
        return analysis::SimilarIncidentSymptom::audio;
    }
    return analysis::SimilarIncidentSymptom::unknown;
}

[[nodiscard]] constexpr analysis::SimilarIncidentFeedback similar_feedback(
    const ui::IncidentFeedback feedback) noexcept {
    switch (feedback) {
    case ui::IncidentFeedback::unanswered:
        return analysis::SimilarIncidentFeedback::unanswered;
    case ui::IncidentFeedback::noticed_problem:
        return analysis::SimilarIncidentFeedback::problem_confirmed;
    case ui::IncidentFeedback::did_not_notice_problem:
        return analysis::SimilarIncidentFeedback::false_positive;
    }
    return analysis::SimilarIncidentFeedback::unanswered;
}

[[nodiscard]] constexpr std::string_view similar_symptom_name(
    const analysis::SimilarIncidentSymptom symptom) noexcept {
    switch (symptom) {
    case analysis::SimilarIncidentSymptom::unknown: return "Unknown";
    case analysis::SimilarIncidentSymptom::system_freeze: return "System freeze";
    case analysis::SimilarIncidentSymptom::game_stutter: return "Game stutter";
    case analysis::SimilarIncidentSymptom::application_slowdown_or_hang:
        return "Application slowdown or hang";
    case analysis::SimilarIncidentSymptom::network: return "Network issue";
    case analysis::SimilarIncidentSymptom::audio: return "Audio issue";
    }
    return "Unknown";
}

[[nodiscard]] std::string context_evidence_text(
    const analysis::WorkloadContextEvidence& evidence) {
    char detail[192]{};
    switch (evidence.signal) {
    case analysis::ContextSignalKind::average_cpu:
        std::snprintf(detail, sizeof(detail), "average CPU %.1f%%",
                      evidence.observed_value * 100.0);
        break;
    case analysis::ContextSignalKind::average_memory:
        std::snprintf(detail, sizeof(detail), "average memory %.1f%%",
                      evidence.observed_value * 100.0);
        break;
    case analysis::ContextSignalKind::average_disk_throughput:
        std::snprintf(detail, sizeof(detail), "average disk throughput %.1f MiB/s",
                      evidence.observed_value / (1024.0 * 1024.0));
        break;
    case analysis::ContextSignalKind::average_network_throughput:
        std::snprintf(detail, sizeof(detail), "average network throughput %.1f MiB/s",
                      evidence.observed_value / (1024.0 * 1024.0));
        break;
    case analysis::ContextSignalKind::low_activity:
        std::snprintf(detail, sizeof(detail), "low-activity signal %.0f%%",
                      evidence.observed_value * 100.0);
        break;
    case analysis::ContextSignalKind::moderate_activity:
        std::snprintf(detail, sizeof(detail), "moderate-activity signal %.0f%%",
                      evidence.observed_value * 100.0);
        break;
    case analysis::ContextSignalKind::process_name_match:
        std::snprintf(detail, sizeof(detail), "%.0f bounded process-name match%s",
                      evidence.observed_value,
                      evidence.observed_value == 1.0 ? "" : "es");
        break;
    case analysis::ContextSignalKind::ambiguous_margin:
        std::snprintf(detail, sizeof(detail), "top workload support margin %.1f%%",
                      evidence.observed_value * 100.0);
        break;
    case analysis::ContextSignalKind::system_sample_coverage:
        std::snprintf(detail, sizeof(detail), "system signal coverage %.1f%%",
                      evidence.observed_value * 100.0);
        break;
    }
    char result[280]{};
    std::snprintf(result, sizeof(result), "%s: %s (support contribution %.1f%%)",
                  context_name(evidence.context).data(), detail,
                  evidence.contribution * 100.0);
    return result;
}

[[nodiscard]] constexpr std::string_view direction_name(
    const analysis::AnomalyDirection direction) noexcept {
    switch (direction) {
    case analysis::AnomalyDirection::unchanged: return "unchanged";
    case analysis::AnomalyDirection::higher: return "higher";
    case analysis::AnomalyDirection::lower: return "lower";
    }
    return "unchanged";
}

[[nodiscard]] double display_value(const analysis::MetricKind metric,
                                   const double value) noexcept {
    switch (metric) {
    case analysis::MetricKind::system_cpu:
    case analysis::MetricKind::system_memory:
    case analysis::MetricKind::process_cpu:
    case analysis::MetricKind::network_tcp_retransmit:
        return value * 100.0;
    case analysis::MetricKind::disk_read_latency:
    case analysis::MetricKind::disk_write_latency:
    case analysis::MetricKind::disk_service_time:
        return value * 1'000.0;
    case analysis::MetricKind::process_working_set:
    case analysis::MetricKind::disk_read:
    case analysis::MetricKind::disk_write:
    case analysis::MetricKind::network_receive:
    case analysis::MetricKind::network_transmit:
    case analysis::MetricKind::process_disk_read:
    case analysis::MetricKind::process_disk_write:
        return value / (1024.0 * 1024.0);
    case analysis::MetricKind::disk_queue_depth:
    case analysis::MetricKind::network_connectivity:
    case analysis::MetricKind::network_interface_changes:
    case analysis::MetricKind::network_tcp_failures:
    case analysis::MetricKind::network_tcp_resets:
        return value;
    }
    return value;
}

[[nodiscard]] constexpr std::string_view display_unit(
    const analysis::MetricKind metric) noexcept {
    switch (metric) {
    case analysis::MetricKind::system_cpu:
    case analysis::MetricKind::system_memory:
    case analysis::MetricKind::process_cpu:
    case analysis::MetricKind::network_tcp_retransmit:
        return "%";
    case analysis::MetricKind::process_working_set:
        return " MiB";
    case analysis::MetricKind::disk_read:
    case analysis::MetricKind::disk_write:
    case analysis::MetricKind::network_receive:
    case analysis::MetricKind::network_transmit:
    case analysis::MetricKind::process_disk_read:
    case analysis::MetricKind::process_disk_write:
        return " MiB/s";
    case analysis::MetricKind::disk_read_latency:
    case analysis::MetricKind::disk_write_latency:
    case analysis::MetricKind::disk_service_time:
        return " ms";
    case analysis::MetricKind::disk_queue_depth:
        return " requests";
    case analysis::MetricKind::network_interface_changes:
    case analysis::MetricKind::network_tcp_failures:
    case analysis::MetricKind::network_tcp_resets:
        return " events";
    case analysis::MetricKind::network_connectivity:
        return " severity";
    }
    return {};
}

[[nodiscard]] const analysis::MetricAnomalyEvidence* strongest_evidence(
    const std::vector<analysis::MetricAnomalyEvidence>& evidence) noexcept {
    const analysis::MetricAnomalyEvidence* result = nullptr;
    for (const auto& item : evidence) {
        if (item.availability == analysis::EvidenceAvailability::available &&
            (result == nullptr || item.score > result->score)) {
            result = &item;
        }
    }
    return result;
}

[[nodiscard]] std::string evidence_text(
    const std::vector<analysis::MetricAnomalyEvidence>& evidence,
    const std::optional<analysis::MetricKind> preferred_metric = std::nullopt) {
    const analysis::MetricAnomalyEvidence* strongest = nullptr;
    if (preferred_metric) {
        const auto found = std::find_if(
            evidence.begin(), evidence.end(), [&](const auto& item) {
                return item.availability == analysis::EvidenceAvailability::available &&
                       item.metric == *preferred_metric;
            });
        if (found != evidence.end()) strongest = &*found;
    } else {
        strongest = strongest_evidence(evidence);
    }
    if (strongest == nullptr) {
        const auto insufficient = std::any_of(
            evidence.begin(), evidence.end(), [](const auto& item) {
                return item.availability ==
                       analysis::EvidenceAvailability::insufficient_baseline;
            });
        return insufficient ? "Insufficient pre-incident baseline"
                            : "Metric unavailable in this incident";
    }
    char text[320]{};
    const auto* scope = strongest->baseline_scope == analysis::BaselineScope::personalized_executable
                            ? "personal executable"
                            : "incident-local";
    std::snprintf(
        text, sizeof(text), "%s %s %s: %.2f%s vs median %.2f%s (P95 %.2f%s, |z| %.2f, n=%zu)",
        scope,
        metric_name(strongest->metric).data(), direction_name(strongest->direction).data(),
        display_value(strongest->metric, strongest->observed_value),
        display_unit(strongest->metric).data(),
        display_value(strongest->metric, strongest->baseline.median),
        display_unit(strongest->metric).data(),
        display_value(strongest->metric, strongest->baseline.p95),
        display_unit(strongest->metric).data(), std::abs(strongest->robust_z),
        strongest->baseline.sample_count);
    return text;
}

void append_context_adjustment(std::string& evidence, const double raw_score,
                               const double multiplier) {
    if (multiplier >= 0.9995 || raw_score <= 0.0) return;
    char text[192]{};
    std::snprintf(text, sizeof(text),
                  "; workload probabilities softly adjusted rank %.1f%% -> %.1f%% "
                  "(x%.3f); raw evidence is unchanged",
                  raw_score * 100.0, raw_score * multiplier * 100.0, multiplier);
    evidence += text;
}

[[nodiscard]] std::string contributor_timing_text(
    const analysis::ContributorCandidate& candidate) {
    char text[384]{};
    if (candidate.temporal_relationship ==
        analysis::ContributorTemporalRelationship::preceding_activity) {
        std::snprintf(text, sizeof(text),
                      "Preceding activity began %.1f s before the marker; "
                      "anomalous span %.1f s (%zu pre / %zu post samples)",
                      std::abs(candidate.activity_started_seconds_from_event),
                      candidate.anomalous_duration_seconds,
                      candidate.pre_marker_anomalous_samples,
                      candidate.post_marker_anomalous_samples);
    } else if (candidate.temporal_relationship ==
               analysis::ContributorTemporalRelationship::
                   marker_spanning_ambiguous) {
        std::snprintf(text, sizeof(text),
                      "Marker-spanning activity began %.1f s before the marker; "
                      "most anomalous samples followed it (%zu pre / %zu post)",
                      std::abs(candidate.activity_started_seconds_from_event),
                      candidate.pre_marker_anomalous_samples,
                      candidate.post_marker_anomalous_samples);
    } else {
        std::snprintf(text, sizeof(text),
                      "Possible victim/reaction began %.1f s after the marker; "
                      "anomalous span %.1f s (0 pre / %zu post samples)",
                      candidate.activity_started_seconds_from_event,
                      candidate.anomalous_duration_seconds,
                      candidate.post_marker_anomalous_samples);
    }
    std::string result{text};
    if (candidate.has_process_start_event) {
        char lifecycle[112]{};
        const auto seconds = candidate.process_started_seconds_from_event;
        std::snprintf(lifecycle, sizeof(lifecycle),
                      "; recorded process start %+.1f s from marker", seconds);
        result += lifecycle;
    }
    if (candidate.has_process_exit_event) {
        char lifecycle[112]{};
        const auto seconds = candidate.process_exited_seconds_from_event;
        std::snprintf(lifecycle, sizeof(lifecycle),
                      "; recorded process exit %+.1f s from marker", seconds);
        result += lifecycle;
    }
    return result;
}

[[nodiscard]] constexpr ui::IncidentContributorRow::TemporalRelationship
ui_temporal_relationship(
    const analysis::ContributorTemporalRelationship relationship) noexcept {
    switch (relationship) {
    case analysis::ContributorTemporalRelationship::preceding_activity:
        return ui::IncidentContributorRow::TemporalRelationship::preceding_activity;
    case analysis::ContributorTemporalRelationship::marker_spanning_ambiguous:
        return ui::IncidentContributorRow::TemporalRelationship::
            marker_spanning_ambiguous;
    case analysis::ContributorTemporalRelationship::post_marker_reaction:
        return ui::IncidentContributorRow::TemporalRelationship::post_marker_reaction;
    }
    return ui::IncidentContributorRow::TemporalRelationship::
        marker_spanning_ambiguous;
}

[[nodiscard]] std::string contributor_assessment_text(
    const analysis::ContributorCandidate& candidate) {
    if (candidate.temporal_relationship ==
        analysis::ContributorTemporalRelationship::post_marker_reaction) {
        return "Possible victim/reaction (not a causal rank)";
    }
    if (candidate.temporal_relationship ==
        analysis::ContributorTemporalRelationship::marker_spanning_ambiguous) {
        return "Ambiguous correlate across marker";
    }
    return candidate.strength == analysis::ContributorStrength::likely
        ? "Likely contributor (correlation only)"
        : "Potential contributor";
}

[[nodiscard]] std::string contributor_evidence_text(
    const analysis::ContributorCandidate& candidate) {
    char text[320]{};
    std::snprintf(
        text, sizeof(text),
        "anomaly %.0f%% + timing %.0f%% + resource match %.0f%% + duration %.0f%% + "
        "recurrence %.0f%% (%zu prior); coverage %.0f%%, %zu missing metric%s",
        candidate.anomaly_magnitude * 100.0, candidate.timing_score * 100.0,
        candidate.resource_match_score * 100.0, candidate.duration_score * 100.0,
        candidate.recurrence_score * 100.0, candidate.recurrence_count,
        candidate.evidence_coverage * 100.0, candidate.missing_metrics,
        candidate.missing_metrics == 1U ? "" : "s");
    return text;
}

struct RecurrenceAnalysisInput {
    analysis::IncidentRecurrenceContext recurrence{};
    std::int64_t current_created_utc_milliseconds{};
    std::vector<analysis::SimilarIncidentFeedbackObservation> feedback_history{};
};

[[nodiscard]] RecurrenceAnalysisInput recurrence_context_for(
    const ui::RecurringIncidentView& recurring, const std::int64_t incident_id) {
    RecurrenceAnalysisInput input{};
    auto& result = input.recurrence;
    if (recurring.state != ui::RecurringIncidentViewState::ready) return input;
    result.available = true;
    for (const auto& group : recurring.groups) {
        const auto member = std::find_if(group.members.begin(), group.members.end(),
                                         [incident_id](const auto& value) {
                                             return value.id == incident_id;
                                         });
        if (member == group.members.end()) continue;
        result.recurring = true;
        result.manually_overridden = group.manually_overridden;
        result.occurrence_count = group.members.size();
        result.shared_characteristic_count = group.shared_characteristic_count;
        result.average_shared_support = group.average_shared_support;
        result.maximum_pair_distance = group.maximum_pair_distance;
        input.current_created_utc_milliseconds = member->created_utc_milliseconds;
        input.feedback_history.reserve(group.members.size());
        for (const auto& candidate : group.members) {
            input.feedback_history.push_back(
                analysis::SimilarIncidentFeedbackObservation{
                    candidate.id,
                    candidate.created_utc_milliseconds,
                    similar_symptom(candidate.category),
                    similar_feedback(candidate.user_feedback)});
        }
        return input;
    }
    const auto noise = std::find_if(recurring.noise.begin(), recurring.noise.end(),
                                    [incident_id](const auto& value) {
                                        return value.id == incident_id;
                                    });
    if (noise != recurring.noise.end()) result.occurrence_count = 1U;
    return input;
}

[[nodiscard]] double seconds_from_event(
    const core::MonotonicTimePoint value,
    const core::MonotonicTimePoint event) noexcept {
    return std::chrono::duration<double>{value - event}.count();
}

struct AnalysisRun {
    std::expected<analysis::IncidentAnalysis, analysis::AnalysisError> result;
    std::string profile_status{};
    std::vector<storage::StoredContributorFeedbackObservation>
        current_contributor_feedback{};
};

[[nodiscard]] constexpr analysis::ResourceKind analysis_resource(
    const storage::ContributorFeedbackResource resource) noexcept {
    switch (resource) {
    case storage::ContributorFeedbackResource::cpu:
        return analysis::ResourceKind::cpu;
    case storage::ContributorFeedbackResource::memory:
        return analysis::ResourceKind::memory;
    case storage::ContributorFeedbackResource::disk:
        return analysis::ResourceKind::disk;
    case storage::ContributorFeedbackResource::network:
        return analysis::ResourceKind::network;
    }
    return analysis::ResourceKind::cpu;
}

[[nodiscard]] constexpr analysis::ContributorTemporalRelationship
analysis_temporal_relationship(
    const storage::ContributorFeedbackTemporalRelationship relationship) noexcept {
    switch (relationship) {
    case storage::ContributorFeedbackTemporalRelationship::preceding_activity:
        return analysis::ContributorTemporalRelationship::preceding_activity;
    case storage::ContributorFeedbackTemporalRelationship::
        marker_spanning_ambiguous:
        return analysis::ContributorTemporalRelationship::marker_spanning_ambiguous;
    case storage::ContributorFeedbackTemporalRelationship::post_marker_reaction:
        return analysis::ContributorTemporalRelationship::post_marker_reaction;
    }
    return analysis::ContributorTemporalRelationship::marker_spanning_ambiguous;
}

[[nodiscard]] constexpr ui::IncidentContributorRow::Resource ui_resource(
    const analysis::ResourceKind resource) noexcept {
    switch (resource) {
    case analysis::ResourceKind::cpu:
        return ui::IncidentContributorRow::Resource::cpu;
    case analysis::ResourceKind::memory:
        return ui::IncidentContributorRow::Resource::memory;
    case analysis::ResourceKind::disk:
        return ui::IncidentContributorRow::Resource::disk;
    case analysis::ResourceKind::network:
        return ui::IncidentContributorRow::Resource::network;
    }
    return ui::IncidentContributorRow::Resource::cpu;
}

[[nodiscard]] AnalysisRun run_analysis(
    analysis::IIncidentAnalyzer* analyzer,
    storage::IProcessProfileRepository* profile_repository,
    storage::IFeedbackCalibrationRepository* feedback_repository,
    const std::int64_t incident_id,
    const core::IncidentSnapshot& incident,
    const RecurrenceAnalysisInput& recurrence_input) {
    if (analyzer == nullptr) {
        return {std::unexpected{analysis::AnalysisError{
                    analysis::AnalysisErrorCode::internal_error, "Analysis disabled"}}, {}, {}};
    }
    std::string profile_status;
    const auto append_status = [&profile_status](std::string value) {
        if (!profile_status.empty()) profile_status += "; ";
        profile_status += std::move(value);
    };
    std::int64_t observed_utc_milliseconds{};
    std::uint64_t feedback_profile_revision{};
    std::int64_t feedback_profile_reset_after{};
    bool feedback_profile_rollback_available{};
    std::vector<analysis::FeedbackObservation> feedback_history;
    if (feedback_repository != nullptr) {
        const auto stored_feedback = feedback_repository->feedback_calibration_context(
            incident_id, storage::maximum_feedback_calibration_observations);
        if (!stored_feedback) {
            append_status("Feedback profile unavailable: " +
                          stored_feedback.error().message);
        } else {
            observed_utc_milliseconds =
                stored_feedback->incident_utc_milliseconds;
            feedback_profile_revision = stored_feedback->profile_revision;
            feedback_profile_reset_after =
                stored_feedback->reset_after_utc_milliseconds;
            feedback_profile_rollback_available =
                stored_feedback->rollback_available;
            feedback_history.reserve(stored_feedback->history.size());
            for (const auto& observation : stored_feedback->history) {
                feedback_history.push_back(analysis::FeedbackObservation{
                    observation.incident_id,
                    observation.observed_utc_milliseconds,
                    observation.automatic_resource,
                    observation.automatic_signal,
                    observation.feedback ==
                            storage::IncidentUserFeedback::did_not_notice_problem
                        ? analysis::FeedbackDisposition::false_positive
                        : analysis::FeedbackDisposition::problem_confirmed});
            }
        }
    }
    if (observed_utc_milliseconds == 0) {
        observed_utc_milliseconds =
            recurrence_input.current_created_utc_milliseconds;
    }
    std::set<std::string, std::less<>> key_set;
    for (const auto& process : incident.process_metadata()) {
        if (const auto identity = analysis::normalize_executable_identity(process)) {
            key_set.insert(identity->key);
            if (key_set.size() == storage::maximum_process_profile_query_identities) break;
        }
    }
    std::vector<std::string> keys{key_set.begin(), key_set.end()};
    std::vector<analysis::ContributorFeedbackObservation>
        contributor_feedback_history;
    std::vector<storage::StoredContributorFeedbackObservation>
        current_contributor_feedback;
    if (feedback_repository != nullptr) {
        const auto stored_contributors =
            feedback_repository->contributor_feedback_context(
                incident_id, keys,
                storage::maximum_contributor_feedback_observations);
        if (!stored_contributors) {
            append_status("Contributor feedback unavailable: " +
                          stored_contributors.error().message);
        } else {
            if (observed_utc_milliseconds == 0) {
                observed_utc_milliseconds =
                    stored_contributors->incident_utc_milliseconds;
            }
            current_contributor_feedback = stored_contributors->current;
            contributor_feedback_history.reserve(
                stored_contributors->history.size());
            for (const auto& observation : stored_contributors->history) {
                contributor_feedback_history.push_back(
                    analysis::ContributorFeedbackObservation{
                        observation.incident_id,
                        observation.incident_utc_milliseconds,
                        observation.feedback_updated_utc_milliseconds,
                        observation.executable_key,
                        analysis_resource(observation.resource),
                        observation.disposition ==
                                storage::ContributorFeedbackDisposition::
                                    confirmed_contributor
                            ? analysis::ContributorFeedbackDisposition::
                                  confirmed_contributor
                            : analysis::ContributorFeedbackDisposition::
                                  not_a_contributor,
                        analysis_temporal_relationship(
                            observation.temporal_relationship)});
            }
        }
    }
    std::vector<analysis::ExecutableProfileObservation> history;
    if (!analyzer->uses_personalized_history() || profile_repository == nullptr) {
        return {analyzer->analyze(
                    incident, analysis::IncidentAnalysisContext{
                                  incident_id, observed_utc_milliseconds, {},
                                  recurrence_input.recurrence,
                                  feedback_history, feedback_profile_revision,
                                  feedback_profile_reset_after,
                                  feedback_profile_rollback_available,
                                  recurrence_input.feedback_history,
                                  contributor_feedback_history}),
                std::move(profile_status),
                std::move(current_contributor_feedback)};
    }
    const auto stored_context = profile_repository->process_profile_context(incident_id, keys);
    if (!stored_context) {
        append_status("Personal history unavailable: " +
                      stored_context.error().message);
        return {analyzer->analyze(
                    incident, analysis::IncidentAnalysisContext{
                                  incident_id, observed_utc_milliseconds, {},
                                  recurrence_input.recurrence,
                                  feedback_history, feedback_profile_revision,
                                  feedback_profile_reset_after,
                                  feedback_profile_rollback_available,
                                  recurrence_input.feedback_history,
                                  contributor_feedback_history}),
                std::move(profile_status),
                std::move(current_contributor_feedback)};
    }
    history.reserve(stored_context->history.size());
    for (const auto& observation : stored_context->history) {
        history.push_back(analysis::ExecutableProfileObservation{
            observation.executable_key, observation.display_name,
            observation.incident_id, observation.observed_utc_milliseconds,
            observation.cpu_fraction, observation.working_set_bytes,
            observation.disk_read_bytes_per_second,
            observation.disk_write_bytes_per_second});
    }
    if (observed_utc_milliseconds == 0) {
        observed_utc_milliseconds = stored_context->incident_utc_milliseconds;
    }
    const analysis::IncidentAnalysisContext context{
        incident_id, observed_utc_milliseconds, history,
        recurrence_input.recurrence,
        feedback_history, feedback_profile_revision,
        feedback_profile_reset_after, feedback_profile_rollback_available,
        recurrence_input.feedback_history, contributor_feedback_history};
    auto analyzed = analyzer->analyze(incident, context);
    if (!analyzed) return {std::move(analyzed), std::move(profile_status),
                           std::move(current_contributor_feedback)};
    std::vector<storage::ProcessProfileUpdate> updates;
    updates.reserve(analyzed->profile_updates.size());
    for (const auto& update : analyzed->profile_updates) {
        updates.push_back(storage::ProcessProfileUpdate{
            update.executable_key, update.display_name, update.cpu_fraction,
            update.working_set_bytes, update.disk_read_bytes_per_second,
            update.disk_write_bytes_per_second});
    }
    if (const auto stored = profile_repository->store_process_profile_updates(
            incident_id, updates); !stored) {
        append_status("Profile update unavailable: " + stored.error().message);
        return {std::move(analyzed), std::move(profile_status),
                std::move(current_contributor_feedback)};
    }
    append_status(updates.empty() ? "No executable profile observations available"
                                  : "Executable profile updated idempotently");
    return {std::move(analyzed), std::move(profile_status),
            std::move(current_contributor_feedback)};
}

[[nodiscard]] std::string diagnosis_evidence_text(
    const analysis::DiagnosisEvidenceLink& link,
    const analysis::IncidentAnalysis& analyzed,
    const core::IncidentSnapshot& incident) {
    char text[384]{};
    switch (link.kind) {
    case analysis::DiagnosisEvidenceKind::resource_anomaly: {
        if (link.source_index >= analyzed.resources.size())
            return "Resource evidence reference unavailable";
        const auto& resource = analyzed.resources[link.source_index];
        const auto detail = evidence_text(resource.evidence);
        std::snprintf(text, sizeof(text),
                      "Recorded %s anomaly %.1f%%: %s; confidence contribution %.1f%%",
                      resource_name(resource.resource).data(), link.source_score * 100.0,
                      detail.c_str(), link.confidence_contribution * 100.0);
        break;
    }
    case analysis::DiagnosisEvidenceKind::process_anomaly: {
        if (link.source_index >= analyzed.processes.size())
            return "Process evidence reference unavailable";
        const auto& process = analyzed.processes[link.source_index];
        const auto detail = evidence_text(process.evidence);
        std::snprintf(text, sizeof(text),
                      "Recorded process evidence for %s (PID %u): %s; referenced without "
                      "double-counting confidence",
                      process.name.c_str(), process.identity.pid, detail.c_str());
        break;
    }
    case analysis::DiagnosisEvidenceKind::contributor_correlation: {
        if (link.source_index >= analyzed.contributors.size())
            return "Contributor evidence reference unavailable";
        const auto& contributor = analyzed.contributors[link.source_index];
        const auto detail = contributor_evidence_text(contributor);
        std::snprintf(text, sizeof(text),
                      "Correlated contributor %s (PID %u), %.1f%%: %s; confidence "
                      "contribution %.1f%%",
                      contributor.name.c_str(), contributor.identity.pid,
                      link.source_score * 100.0, detail.c_str(),
                      link.confidence_contribution * 100.0);
        break;
    }
    case analysis::DiagnosisEvidenceKind::workload_context: {
        if (link.source_index >= analyzed.workload_context.probabilities.size())
            return "Workload-context evidence reference unavailable";
        const auto& probability = analyzed.workload_context.probabilities[link.source_index];
        std::snprintf(text, sizeof(text),
                      "Probabilistic workload context %s %.1f%%; confidence contribution %.1f%%",
                      context_name(probability.context).data(),
                      probability.probability * 100.0,
                      link.confidence_contribution * 100.0);
        break;
    }
    case analysis::DiagnosisEvidenceKind::recurring_pattern:
        std::snprintf(text, sizeof(text),
                      "Automatic recurring pattern across %zu incidents, maximum pair distance "
                      "%.3f, %zu shared characteristics; confidence contribution %.1f%%",
                      analyzed.recurrence.occurrence_count,
                      analyzed.recurrence.maximum_pair_distance,
                      analyzed.recurrence.shared_characteristic_count,
                      link.confidence_contribution * 100.0);
        break;
    case analysis::DiagnosisEvidenceKind::automatic_capture_trigger:
        std::snprintf(text, sizeof(text),
                      "Recorded automatic capture trigger score %.1f%% from %u trigger%s; "
                      "confidence contribution %.1f%%",
                      link.source_score * 100.0,
                      incident.header().window.automatic_trigger_count,
                      incident.header().window.automatic_trigger_count == 1U ? "" : "s",
                      link.confidence_contribution * 100.0);
        break;
    case analysis::DiagnosisEvidenceKind::system_event: {
        if (link.source_index >= incident.system_events().size())
            return "System-event evidence reference unavailable";
        const auto& event = incident.system_events()[link.source_index];
        const auto event_name = event.kind == core::SystemEventKind::display_driver_recovery
            ? "Display timeout recovery"
            : event.kind == core::SystemEventKind::storage_io_retry
            ? "Disk I/O retry"
            : event.kind == core::SystemEventKind::dns_resolution_timeout
            ? "DNS Client resolution-timeout"
            : event.kind == core::SystemEventKind::application_crash
            ? "Application Error"
            : event.kind == core::SystemEventKind::application_hang
            ? "Application Hang"
            : "normalized system";
        std::snprintf(text, sizeof(text),
                      "Windows recorded %s event %u within five seconds of the marker; confidence contribution %.1f%%",
                      event_name, event.native_event_id,
                      link.confidence_contribution * 100.0);
        break;
    }
    }
    return text;
}

[[nodiscard]] ui::IncidentAnalysisView build_analysis_view(
    const AnalysisRun& run,
    const core::IncidentSnapshot& incident) {
    ui::IncidentAnalysisView view{};
    const auto& analyzed = run.result;
    if (!analyzed) {
        view.state = ui::IncidentAnalysisViewState::error;
        view.status = analyzed.error().message;
        return view;
    }
    const auto event = incident.header().window.event_time;
    view.state = analyzed->cold_start ? ui::IncidentAnalysisViewState::cold_start
                                      : ui::IncidentAnalysisViewState::ready;
    const auto personalized_ready = std::any_of(
        analyzed->processes.begin(), analyzed->processes.end(), [](const auto& process) {
            return process.personalization == analysis::PersonalizationState::ready;
        });
    const auto personalized_cold = std::any_of(
        analyzed->processes.begin(), analyzed->processes.end(), [](const auto& process) {
            return process.personalization == analysis::PersonalizationState::cold_start;
        });
    view.status = analyzed->cold_start
                      ? "Cold start: insufficient pre-incident system baseline"
                      : personalized_ready
                            ? "Personalized executable ranking with aging history"
                            : personalized_cold
                                  ? "Personal profiles are cold; process ranks remain incident-local"
                                  : "Explainable incident-local statistical ranking";
    if (!run.profile_status.empty()) view.status += ". " + run.profile_status;
    view.baseline_start_seconds = seconds_from_event(analyzed->baseline_start, event);
    view.baseline_end_seconds = seconds_from_event(analyzed->baseline_end, event);
    view.evaluation_start_seconds = seconds_from_event(analyzed->evaluation_start, event);
    view.evaluation_end_seconds = seconds_from_event(analyzed->evaluation_end, event);
    view.missing_values = analyzed->missing_values;
    const auto pressure = std::max_element(
        analyzed->resources.begin(), analyzed->resources.end(),
        [](const auto& left, const auto& right) {
            if (left.score != right.score) return left.score < right.score;
            return left.resource > right.resource;
        });
    if (pressure != analyzed->resources.end() && pressure->score > 0.0 &&
        pressure->pressure_metric) {
        view.pressure.available = true;
        view.pressure.resource = std::string{resource_name(pressure->resource)};
        view.pressure.metric = std::string{metric_name(*pressure->pressure_metric)};
        view.pressure.score = pressure->score;
        view.pressure.statistical_score = pressure->statistical_score;
        view.pressure.evidence = evidence_text(
            pressure->evidence, pressure->pressure_metric);
    }
    view.diagnosis.available = analyzed->diagnosis.available;
    view.diagnosis.incident_type = std::string{
        incident_type_name(analyzed->diagnosis.type)};
    view.diagnosis.basis = std::string{
        explanation_basis_name(analyzed->diagnosis.basis)};
    view.diagnosis.confidence = std::string{
        confidence_name(analyzed->diagnosis.confidence)};
    view.diagnosis.calibrated_confidence =
        analyzed->diagnosis.calibrated_confidence;
    view.diagnosis.evidence_coverage = analyzed->diagnosis.evidence_coverage;
    view.diagnosis.correlated_evidence_penalty =
        analyzed->diagnosis.correlated_evidence_penalty;
    view.diagnosis.confidence_before_feedback =
        analyzed->diagnosis.confidence_before_feedback;
    view.diagnosis.feedback_multiplier = analyzed->diagnosis.feedback_multiplier;
    view.diagnosis.suppressed_by_feedback =
        analyzed->diagnosis.suppressed_by_feedback;
    view.diagnosis.pipeline_version = analyzed->provenance.pipeline_version;
    view.diagnosis.evidence_model_version =
        analyzed->provenance.evidence_model_version;
    view.diagnosis.configuration_fingerprint =
        analyzed->provenance.configuration_fingerprint;
    view.diagnosis.inference = "Statistical only; native ML not adopted";
    if (analyzed->diagnosis.primary_contributor_index &&
        *analyzed->diagnosis.primary_contributor_index < analyzed->contributors.size()) {
        const auto& contributor = analyzed->contributors[
            *analyzed->diagnosis.primary_contributor_index];
        view.diagnosis.primary_contributor = contributor.name + " (PID " +
                                             std::to_string(contributor.identity.pid) + ")";
    }
    view.diagnosis.evidence.reserve(analyzed->diagnosis.evidence.size());
    for (const auto& evidence : analyzed->diagnosis.evidence) {
        view.diagnosis.evidence.push_back(
            diagnosis_evidence_text(evidence, *analyzed, incident));
    }
    const auto& feedback = analyzed->feedback_calibration;
    view.feedback.applicable = feedback.state !=
        analysis::FeedbackCalibrationState::not_applicable;
    view.feedback.ready = feedback.state == analysis::FeedbackCalibrationState::stable ||
                          feedback.state ==
                              analysis::FeedbackCalibrationState::suppressing;
    view.feedback.suppressing = feedback.state ==
        analysis::FeedbackCalibrationState::suppressing;
    view.feedback.observations_considered = feedback.observations_considered;
    view.feedback.matching_observations = feedback.matching_observations;
    view.feedback.confirmed_problem_observations =
        feedback.confirmed_problem_observations;
    view.feedback.false_positive_observations =
        feedback.false_positive_observations;
    view.feedback.false_positive_fraction = feedback.false_positive_fraction;
    view.feedback.confidence_multiplier = feedback.confidence_multiplier;
    view.feedback.profile_revision = feedback.profile_revision;
    view.feedback.reset_after_utc_milliseconds =
        feedback.reset_after_utc_milliseconds;
    view.feedback.rollback_available = feedback.rollback_available;
    if (feedback.state == analysis::FeedbackCalibrationState::cold_start) {
        view.feedback.status = "Feedback profile cold start: " +
            std::to_string(feedback.matching_observations) +
            " matching answered incidents; at least 4 are required";
    } else if (feedback.state == analysis::FeedbackCalibrationState::stable) {
        view.feedback.status = "Feedback profile retained the unadjusted assertion: " +
            std::to_string(feedback.false_positive_observations) + " of " +
            std::to_string(feedback.matching_observations) +
            " matching triggers were not noticed";
    } else if (feedback.state == analysis::FeedbackCalibrationState::suppressing) {
        char status[256]{};
        std::snprintf(status, sizeof(status),
                      "Feedback profile reduced automatic-trigger confidence x%.3f: %zu of %zu matching triggers were not noticed",
                      feedback.confidence_multiplier,
                      feedback.false_positive_observations,
                      feedback.matching_observations);
        view.feedback.status = status;
    }
    const auto& similar = analyzed->similar_incident_evidence;
    view.similar_incidents.applicable =
        similar.state != analysis::SimilarIncidentEvidenceState::not_applicable;
    view.similar_incidents.ready =
        similar.state == analysis::SimilarIncidentEvidenceState::ready;
    view.similar_incidents.manually_excluded =
        similar.state ==
        analysis::SimilarIncidentEvidenceState::manual_group_excluded;
    view.similar_incidents.symptom = std::string{
        similar_symptom_name(similar.symptom)};
    view.similar_incidents.observations_considered =
        similar.observations_considered;
    view.similar_incidents.answered_observations = similar.answered_observations;
    view.similar_incidents.confirmed_problem_observations =
        similar.confirmed_problem_observations;
    view.similar_incidents.false_positive_observations =
        similar.false_positive_observations;
    view.similar_incidents.categorized_confirmations =
        similar.categorized_confirmations;
    view.similar_incidents.matching_confirmations =
        similar.matching_confirmations;
    view.similar_incidents.problem_fraction = similar.problem_fraction;
    view.similar_incidents.category_consensus = similar.category_consensus;
    if (similar.state ==
        analysis::SimilarIncidentEvidenceState::manual_group_excluded) {
        view.similar_incidents.status =
            "User-created recurrence groups are excluded from learned context";
    } else if (similar.state ==
               analysis::SimilarIncidentEvidenceState::cold_start) {
        view.similar_incidents.status =
            "Confirmed similar-incident context is cold: " +
            std::to_string(similar.matching_confirmations) +
            " matching prior confirmations; at least 2 are required";
    } else if (similar.state ==
               analysis::SimilarIncidentEvidenceState::conflicting) {
        char status[320]{};
        std::snprintf(
            status, sizeof(status),
            "Prior feedback is conflicting: %.0f%% confirmed a problem and %.0f%% of categorized confirmations agree on %s",
            similar.problem_fraction * 100.0,
            similar.category_consensus * 100.0,
            similar_symptom_name(similar.symptom).data());
        view.similar_incidents.status = status;
    } else if (similar.state == analysis::SimilarIncidentEvidenceState::ready) {
        char status[320]{};
        std::snprintf(
            status, sizeof(status),
            "%zu of %zu categorized prior confirmations agree on %s; %zu of %zu answered similar incidents confirmed a noticed problem",
            similar.matching_confirmations,
            similar.categorized_confirmations,
            similar_symptom_name(similar.symptom).data(),
            similar.confirmed_problem_observations,
            similar.answered_observations);
        view.similar_incidents.status = status;
    }
    view.context.enabled = analyzed->workload_context.enabled;
    view.context.primary = std::string{context_name(analyzed->workload_context.primary)};
    view.context.confidence = analyzed->workload_context.confidence;
    view.context.uncertainty = analyzed->workload_context.uncertainty;
    view.context.probabilities.reserve(analyzed->workload_context.probabilities.size());
    for (const auto& probability : analyzed->workload_context.probabilities) {
        view.context.probabilities.push_back({
            std::string{context_name(probability.context)}, probability.probability});
    }
    view.context.evidence.reserve(analyzed->workload_context.evidence.size());
    for (const auto& evidence : analyzed->workload_context.evidence) {
        view.context.evidence.push_back(context_evidence_text(evidence));
    }
    view.contributors.reserve(analyzed->contributors.size());
    for (const auto& item : analyzed->contributors) {
        ui::IncidentContributorRow row{
            item.name + " (PID " + std::to_string(item.identity.pid) + ")",
            item.score,
            contributor_assessment_text(item),
            contributor_timing_text(item), contributor_evidence_text(item)};
        row.executable_key = item.executable_key;
        row.resource = ui_resource(item.matched_resource);
        row.temporal_relationship =
            ui_temporal_relationship(item.temporal_relationship);
        row.score_before_feedback = item.score_before_feedback;
        row.feedback_multiplier = item.feedback_multiplier;
        row.feedback_matching_observations =
            item.feedback_matching_observations;
        row.feedback_confirmed_observations =
            item.feedback_confirmed_observations;
        row.feedback_rejected_observations =
            item.feedback_rejected_observations;
        const auto current = std::find_if(
            run.current_contributor_feedback.begin(),
            run.current_contributor_feedback.end(), [&](const auto& feedback) {
                return feedback.executable_key == item.executable_key &&
                       analysis_resource(feedback.resource) == item.matched_resource;
            });
        if (current != run.current_contributor_feedback.end()) {
            row.attribution = current->disposition ==
                                      storage::ContributorFeedbackDisposition::
                                          confirmed_contributor
                                  ? ui::IncidentContributorRow::Attribution::
                                        confirmed_contributor
                                  : ui::IncidentContributorRow::Attribution::
                                        not_a_contributor;
        }
        switch (item.feedback_state) {
        case analysis::ContributorFeedbackState::not_applicable:
            row.feedback_status = item.executable_key.empty()
                ? "No stable executable identity; cannot learn"
                : "No prior explicit attribution applies";
            break;
        case analysis::ContributorFeedbackState::cold_start:
            row.feedback_status = "Attribution history cold: " +
                std::to_string(item.feedback_matching_observations) +
                " exact prior match(es); 4 required";
            break;
        case analysis::ContributorFeedbackState::conflicting:
            row.feedback_status = "Conflicting exact-match attribution retained without influence";
            break;
        case analysis::ContributorFeedbackState::stable:
            row.feedback_status =
                "Confirmed consensus inspected; post-marker activity was not promoted";
            break;
        case analysis::ContributorFeedbackState::promoted:
        case analysis::ContributorFeedbackState::reduced: {
            char status[256]{};
            std::snprintf(
                status, sizeof(status),
                "%s x%.3f from %zu confirmed / %zu rejected exact prior attribution(s)",
                item.feedback_state ==
                        analysis::ContributorFeedbackState::promoted
                    ? "Bounded promotion"
                    : "Bounded reduction",
                item.feedback_multiplier,
                item.feedback_confirmed_observations,
                item.feedback_rejected_observations);
            row.feedback_status = status;
            break;
        }
        }
        view.contributors.push_back(std::move(row));
    }
    view.resources.reserve(analyzed->resources.size());
    for (const auto& item : analyzed->resources) {
        auto evidence = evidence_text(item.evidence);
        append_context_adjustment(evidence, item.uncontextualized_score,
                                  item.context_multiplier);
        view.resources.push_back(ui::IncidentAnomalyRow{
            std::string{resource_name(item.resource)}, item.score,
            std::string{confidence_name(item.confidence)}, std::move(evidence)});
    }
    const auto process_count = (std::min)(
        analyzed->processes.size(), ui::incident_analysis_process_capacity);
    view.processes.reserve(process_count);
    for (std::size_t index = 0U; index < process_count; ++index) {
        const auto& item = analyzed->processes[index];
        auto evidence = evidence_text(item.evidence);
        append_context_adjustment(evidence, item.uncontextualized_score,
                                  item.context_multiplier);
        if (item.personalization == analysis::PersonalizationState::cold_start) {
            evidence = "Personal profile cold start (" +
                       std::to_string(item.personalized_observations) +
                       " observations); " + evidence;
        }
        auto confidence = std::string{confidence_name(item.confidence)};
        if (item.personalization == analysis::PersonalizationState::ready) {
            confidence += " / personalized";
        } else if (item.personalization == analysis::PersonalizationState::cold_start) {
            confidence += " / profile cold start";
        }
        view.processes.push_back(ui::IncidentAnomalyRow{
            item.name + " (PID " + std::to_string(item.identity.pid) + ")",
            item.score, std::move(confidence), std::move(evidence)});
    }
    return view;
}

[[nodiscard]] constexpr std::string_view feature_dimension_name(
    const analysis::IncidentFeatureDimension dimension) noexcept {
    switch (dimension) {
    case analysis::IncidentFeatureDimension::cpu_peak: return "CPU peak";
    case analysis::IncidentFeatureDimension::cpu_near_marker: return "CPU near marker";
    case analysis::IncidentFeatureDimension::memory_peak: return "memory peak";
    case analysis::IncidentFeatureDimension::memory_near_marker:
        return "memory near marker";
    case analysis::IncidentFeatureDimension::disk_peak: return "disk peak";
    case analysis::IncidentFeatureDimension::disk_near_marker: return "disk near marker";
    case analysis::IncidentFeatureDimension::network_peak: return "network peak";
    case analysis::IncidentFeatureDimension::network_near_marker:
        return "network near marker";
    case analysis::IncidentFeatureDimension::dominant_pre_marker_share:
        return "pre-marker share";
    case analysis::IncidentFeatureDimension::dominant_post_marker_share:
        return "post-marker share";
    case analysis::IncidentFeatureDimension::duration: return "duration shape";
    case analysis::IncidentFeatureDimension::dominant_resource_concentration:
        return "dominant resource concentration";
    case analysis::IncidentFeatureDimension::disk_quality_peak:
        return "physical-disk stall peak";
    case analysis::IncidentFeatureDimension::disk_quality_near_marker:
        return "physical-disk stall near marker";
    case analysis::IncidentFeatureDimension::network_quality_peak:
        return "network disruption peak";
    case analysis::IncidentFeatureDimension::network_quality_near_marker:
        return "network disruption near marker";
    }
    return "feature";
}

[[nodiscard]] std::string shared_characteristics_text(
    const analysis::IncidentCluster& cluster) {
    if (cluster.shared_characteristics.empty()) return "Insufficient shared feature coverage";
    std::string result;
    for (const auto& characteristic : cluster.shared_characteristics) {
        if (!result.empty()) result += "; ";
        char value[80]{};
        std::snprintf(value, sizeof(value), "%s %.0f%% (%.0f%% support)",
                      feature_dimension_name(characteristic.dimension).data(),
                      characteristic.median * 100.0,
                      characteristic.support * 100.0);
        result += value;
    }
    return result;
}
#endif

} // namespace

IncidentViewerService::IncidentViewerService(
    storage::IIncidentRepository& repository,
    analysis::IIncidentAnalyzer* analyzer,
    storage::IProcessProfileRepository* profile_repository,
    storage::IFeedbackCalibrationRepository* feedback_repository,
    storage::IRecurringIncidentRepository* recurring_repository) noexcept
    : repository_{repository}, analyzer_{analyzer},
      profile_repository_{profile_repository},
      feedback_repository_{feedback_repository},
      recurring_repository_{recurring_repository} {}

IncidentViewerService::~IncidentViewerService() {
    stop();
}

void IncidentViewerService::start() {
    const std::scoped_lock lock{mutex_};
    if (worker_.joinable()) return;
    worker_ = std::jthread{[this](const std::stop_token stop_token) { run(stop_token); }};
}

void IncidentViewerService::stop() noexcept {
    std::jthread worker;
    {
        const std::scoped_lock lock{mutex_};
        if (!worker_.joinable()) return;
        worker_.request_stop();
        available_.notify_all();
        worker = std::move(worker_);
    }
    if (worker.joinable()) worker.join();
}

void IncidentViewerService::request_page(const std::size_t offset, std::string search,
                                         const ui::IncidentListOrder order) {
    Job job{};
    job.type = JobType::page;
    job.offset = offset;
    job.search = std::move(search);
    job.order = order;
    enqueue(std::move(job));
}

void IncidentViewerService::request_detail(const std::int64_t incident_id) {
    Job job{};
    job.type = JobType::detail;
    job.incident_id = incident_id;
    enqueue(std::move(job));
}

void IncidentViewerService::request_process(
    const std::int64_t incident_id, const core::IncidentProcessIdentity identity) {
    Job job{};
    job.type = JobType::process;
    job.incident_id = incident_id;
    job.identity = identity;
    enqueue(std::move(job));
}

void IncidentViewerService::update_annotation(const std::int64_t incident_id,
                                              std::string label, std::string note,
                                              const storage::IncidentUserFeedback feedback,
                                              const storage::IncidentCategory category) {
    Job job{};
    job.type = JobType::annotation;
    job.incident_id = incident_id;
    job.label = std::move(label);
    job.note = std::move(note);
    job.feedback = feedback;
    job.category = category;
    enqueue(std::move(job));
}

void IncidentViewerService::update_contributor_feedback(
    const std::int64_t incident_id, std::string executable_key,
    const storage::ContributorFeedbackResource resource,
    const storage::ContributorFeedbackDisposition disposition,
    const storage::ContributorFeedbackTemporalRelationship temporal_relationship) {
    Job job{};
    job.type = JobType::contributor_feedback;
    job.incident_id = incident_id;
    job.contributor_executable_key = std::move(executable_key);
    job.contributor_resource = resource;
    job.contributor_disposition = disposition;
    job.contributor_temporal_relationship = temporal_relationship;
    enqueue(std::move(job));
}

void IncidentViewerService::request_recurring_incidents() {
    Job job{};
    job.type = JobType::recurring;
    enqueue(std::move(job));
}

void IncidentViewerService::update_recurring_group_override(
    const std::int64_t incident_id, std::string override_group) {
    Job job{};
    job.type = JobType::recurring_override;
    job.incident_id = incident_id;
    job.recurring_group_override = std::move(override_group);
    enqueue(std::move(job));
}

void IncidentViewerService::reset_feedback_profile() {
    Job job{};
    job.type = JobType::feedback_reset;
    enqueue(std::move(job));
}

void IncidentViewerService::rollback_feedback_profile_reset() {
    Job job{};
    job.type = JobType::feedback_rollback;
    enqueue(std::move(job));
}

std::shared_ptr<const ui::IncidentViewerContent> IncidentViewerService::snapshot() const {
    const std::scoped_lock lock{mutex_};
    return snapshot_;
}

void IncidentViewerService::enqueue(Job job) {
    const std::scoped_lock lock{mutex_};
    constexpr std::size_t maximum_jobs = 8U;
    if (jobs_.size() == maximum_jobs) jobs_.pop_front();
    jobs_.push_back(std::move(job));
    available_.notify_one();
}

void IncidentViewerService::run(const std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
        Job job{};
        {
            std::unique_lock lock{mutex_};
            static_cast<void>(available_.wait(
                lock, stop_token, [this] { return !jobs_.empty(); }));
            if (jobs_.empty()) continue;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        try {
            switch (job.type) {
            case JobType::page: handle_page(job); break;
            case JobType::detail: handle_detail(job); break;
            case JobType::process: handle_process(job); break;
            case JobType::annotation: handle_annotation(job); break;
            case JobType::contributor_feedback:
                handle_contributor_feedback(job);
                break;
            case JobType::recurring: handle_recurring(); break;
            case JobType::recurring_override: handle_recurring_override(job); break;
            case JobType::feedback_reset: handle_feedback_reset(); break;
            case JobType::feedback_rollback: handle_feedback_rollback(); break;
            }
        } catch (const std::exception& exception) {
            publish_error(exception.what());
        } catch (...) {
            publish_error("Unknown incident viewer failure");
        }
    }
}

storage::IncidentListQuery IncidentViewerService::storage_query(const Job& job) const {
    storage::IncidentListQuery query{};
    query.offset = job.offset;
    query.limit = ui::incident_list_page_size;
    query.search = job.search;
    query.sort = storage_order(job.order);
    return query;
}

void IncidentViewerService::handle_page(const Job& job) {
    const auto query = storage_query(job);
    const auto started = std::chrono::steady_clock::now();
    auto page = repository_.list_page(query);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (!page) {
        publish_error(page.error().message);
        return;
    }
    last_page_ = *page;
    last_query_ = query;
    auto content = *snapshot();
    content.state = ui::IncidentViewerLoadState::ready;
    content.status = std::to_string(page->total_matching) + " matching incidents";
    content.incidents.clear();
    content.incidents.reserve(page->incidents.size());
    for (const auto& incident : page->incidents) {
        ui::IncidentListRow row{};
        row.id = incident.id;
        row.created_utc_milliseconds = incident.created_utc_milliseconds;
        row.created_utc = ui::format_utc_milliseconds(incident.created_utc_milliseconds);
        row.duration_seconds = static_cast<double>(incident.actual_end_nanoseconds -
                                                   incident.actual_start_nanoseconds) /
                               1'000'000'000.0;
        row.label = incident.label;
        row.note_preview = note_preview(incident.note);
        row.system_sample_count = incident.system_sample_count;
        row.process_sample_count = incident.process_sample_count;
        content.incidents.push_back(std::move(row));
    }
    content.total_matching = page->total_matching;
    content.offset = page->offset;
    content.last_query_milliseconds = milliseconds(elapsed);
    publish(std::move(content));
}

void IncidentViewerService::handle_detail(const Job& job) {
    const auto query_started = std::chrono::steady_clock::now();
    auto incident = repository_.load(job.incident_id);
    if (!incident) {
        publish_error(incident.error().message);
        return;
    }
    auto annotation = repository_.annotation(job.incident_id);
    const auto query_elapsed = std::chrono::steady_clock::now() - query_started;
    if (!annotation) {
        publish_error(annotation.error().message);
        return;
    }
    loaded_incident_ = *incident;
    loaded_annotation_ = *annotation;
    loaded_incident_id_ = job.incident_id;
    loaded_created_utc_milliseconds_ = 0;
    const auto summary = std::find_if(last_page_.incidents.begin(), last_page_.incidents.end(),
                                      [&](const storage::StoredIncidentSummary& value) {
                                          return value.id == job.incident_id;
                                      });
    if (summary != last_page_.incidents.end()) {
        loaded_created_utc_milliseconds_ = summary->created_utc_milliseconds;
    } else if (const auto recurring = recurring_created_utc_by_id_.find(job.incident_id);
               recurring != recurring_created_utc_by_id_.end()) {
        loaded_created_utc_milliseconds_ = recurring->second;
    }
    std::string recurring_override;
    if (recurring_repository_ != nullptr) {
        if (auto loaded_override = recurring_repository_->recurring_group_override(
                job.incident_id); loaded_override) {
            recurring_override = std::move(*loaded_override);
        }
    }
#if BLACKBOX_ANALYSIS_ENABLED
    const auto analysis_started = std::chrono::steady_clock::now();
    if (analyzer_ == nullptr) {
        loaded_analysis_ = ui::IncidentAnalysisView{};
    } else {
        const auto recurrence = recurrence_context_for(
            snapshot()->recurring, job.incident_id);
        const auto analysis_run = run_analysis(analyzer_, profile_repository_,
                                               feedback_repository_,
                                               job.incident_id, **incident,
                                               recurrence);
        loaded_analysis_ = build_analysis_view(analysis_run, **incident);
    }
    const auto analysis_elapsed = std::chrono::steady_clock::now() - analysis_started;
#else
    loaded_analysis_ = ui::IncidentAnalysisView{};
#endif
    const auto build_started = std::chrono::steady_clock::now();
    auto detail = ui::build_incident_detail(
        job.incident_id, loaded_created_utc_milliseconds_, annotation->label,
        annotation->note, **incident);
    detail.user_feedback = static_cast<ui::IncidentFeedback>(annotation->user_feedback);
    detail.category = static_cast<ui::IncidentCategory>(annotation->category);
    detail.recurring_group_override = std::move(recurring_override);
    detail.analysis = loaded_analysis_;
    const auto build_elapsed = std::chrono::steady_clock::now() - build_started;
    auto content = *snapshot();
    content.state = ui::IncidentViewerLoadState::ready;
    content.status = "Incident loaded";
    content.detail = std::move(detail);
    content.last_query_milliseconds = milliseconds(query_elapsed);
    content.last_build_milliseconds = milliseconds(build_elapsed);
#if BLACKBOX_ANALYSIS_ENABLED
    content.last_analysis_milliseconds = milliseconds(analysis_elapsed);
#else
    content.last_analysis_milliseconds = 0.0;
#endif
    publish(std::move(content));
}

void IncidentViewerService::handle_process(const Job& job) {
    if (loaded_incident_id_ != job.incident_id || !loaded_incident_) {
        handle_detail(Job{.type = JobType::detail, .incident_id = job.incident_id});
        if (loaded_incident_id_ != job.incident_id || !loaded_incident_) return;
    }
    const auto build_started = std::chrono::steady_clock::now();
    auto detail = ui::build_incident_detail(
        job.incident_id, loaded_created_utc_milliseconds_, loaded_annotation_.label,
        loaded_annotation_.note, *loaded_incident_, job.identity);
    detail.user_feedback = static_cast<ui::IncidentFeedback>(
        loaded_annotation_.user_feedback);
    detail.category = static_cast<ui::IncidentCategory>(loaded_annotation_.category);
    const auto previous = snapshot();
    if (previous->detail && previous->detail->id == job.incident_id) {
        detail.recurring_group_override =
            previous->detail->recurring_group_override;
    }
    detail.analysis = loaded_analysis_;
    auto content = *snapshot();
    content.detail = std::move(detail);
    content.status = "Process timeline loaded";
    content.last_build_milliseconds = milliseconds(
        std::chrono::steady_clock::now() - build_started);
    publish(std::move(content));
}

void IncidentViewerService::handle_annotation(const Job& job) {
    const storage::IncidentAnnotation annotation{
        job.label, job.note, job.feedback, job.category};
    const auto started = std::chrono::steady_clock::now();
    auto updated = repository_.update_annotation(job.incident_id, annotation);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (!updated) {
        publish_error(updated.error().message);
        return;
    }
    loaded_annotation_ = annotation;
    for (auto& summary : last_page_.incidents) {
        if (summary.id == job.incident_id) {
            summary.label = job.label;
            summary.note = job.note;
        }
    }
    auto content = *snapshot();
    for (auto& row : content.incidents) {
        if (row.id == job.incident_id) {
            row.label = job.label;
            row.note_preview = note_preview(job.note);
        }
    }
    if (content.detail && content.detail->id == job.incident_id) {
        content.detail->label = job.label;
        content.detail->note = job.note;
        content.detail->user_feedback = static_cast<ui::IncidentFeedback>(job.feedback);
        content.detail->category = static_cast<ui::IncidentCategory>(job.category);
    }
    content.state = ui::IncidentViewerLoadState::ready;
    content.status = "Incident details saved";
    content.last_query_milliseconds = milliseconds(elapsed);
    publish(std::move(content));
}

void IncidentViewerService::handle_contributor_feedback(const Job& job) {
    if (feedback_repository_ == nullptr) {
        publish_error("Contributor feedback storage is unavailable");
        return;
    }
    const auto updated = feedback_repository_->update_contributor_feedback(
        job.incident_id, job.contributor_executable_key,
        job.contributor_resource, job.contributor_disposition,
        job.contributor_temporal_relationship);
    if (!updated) {
        publish_error(updated.error().message);
        return;
    }
    handle_detail(Job{.type = JobType::detail,
                      .incident_id = job.incident_id});
    auto content = *snapshot();
    content.state = ui::IncidentViewerLoadState::ready;
    content.status =
        job.contributor_disposition ==
                storage::ContributorFeedbackDisposition::unsure
            ? "Contributor attribution cleared"
            : job.contributor_disposition ==
                      storage::ContributorFeedbackDisposition::confirmed_contributor &&
                  job.contributor_temporal_relationship !=
                      storage::ContributorFeedbackTemporalRelationship::
                          preceding_activity
                ? "Attribution saved for this incident; non-preceding confirmation cannot teach positive uplift"
                : "Contributor attribution saved for future exact matches";
    publish(std::move(content));
}

void IncidentViewerService::handle_feedback_reset() {
    if (feedback_repository_ == nullptr) {
        publish_error("Feedback profile storage is unavailable");
        return;
    }
    const auto reset = feedback_repository_->reset_feedback_profile();
    if (!reset) {
        publish_error(reset.error().message);
        return;
    }
    if (loaded_incident_id_ != 0) {
        handle_detail(Job{.type = JobType::detail,
                          .incident_id = loaded_incident_id_});
    }
    auto content = *snapshot();
    content.state = ui::IncidentViewerLoadState::ready;
    content.status = "Feedback influence reset; prior annotations remain stored";
    publish(std::move(content));
}

void IncidentViewerService::handle_feedback_rollback() {
    if (feedback_repository_ == nullptr) {
        publish_error("Feedback profile storage is unavailable");
        return;
    }
    const auto rolled_back =
        feedback_repository_->rollback_feedback_profile_reset();
    if (!rolled_back) {
        publish_error(rolled_back.error().message);
        return;
    }
    if (loaded_incident_id_ != 0) {
        handle_detail(Job{.type = JobType::detail,
                          .incident_id = loaded_incident_id_});
    }
    auto content = *snapshot();
    content.state = ui::IncidentViewerLoadState::ready;
    content.status = "Feedback influence reset rolled back";
    publish(std::move(content));
}

void IncidentViewerService::handle_recurring() {
    const auto started = std::chrono::steady_clock::now();
    auto content = *snapshot();
#if BLACKBOX_ANALYSIS_ENABLED
    if (recurring_repository_ == nullptr) {
        content.recurring.state = ui::RecurringIncidentViewState::disabled;
        content.recurring.status = "Recurring storage unavailable";
        publish(std::move(content));
        return;
    }
    const auto records = recurring_repository_->recurring_incidents(
        storage::maximum_recurring_incidents);
    if (!records) {
        content.recurring.state = ui::RecurringIncidentViewState::error;
        content.recurring.status = records.error().message;
        content.recurring.elapsed_milliseconds = milliseconds(
            std::chrono::steady_clock::now() - started);
        publish(std::move(content));
        return;
    }
    std::vector<analysis::IncidentClusterInput> inputs;
    std::vector<storage::StoredIncidentFeatureCache> updates;
    inputs.reserve(records->size());
    updates.reserve(records->size());
    recurring_created_utc_by_id_.clear();
    std::size_t cached_count{};
    std::size_t load_failures{};
    for (const auto& record : *records) {
        recurring_created_utc_by_id_.emplace(record.id,
                                             record.created_utc_milliseconds);
        analysis::IncidentFeatureVector feature{};
        auto cache_valid = record.cached_feature.has_value() &&
            record.cached_feature->feature_version == analysis::incident_feature_version &&
            record.cached_feature->values.size() ==
                analysis::incident_feature_dimension_count &&
            record.cached_feature->available.size() ==
                analysis::incident_feature_dimension_count;
        if (cache_valid) {
            feature.incident_id = record.id;
            feature.created_utc_milliseconds = record.created_utc_milliseconds;
            for (std::size_t index = 0U;
                 index < analysis::incident_feature_dimension_count; ++index) {
                feature.values[index] = record.cached_feature->values[index];
                feature.available[index] = record.cached_feature->available[index] != 0U;
            }
            ++cached_count;
        } else {
            auto incident = repository_.load(record.id);
            if (!incident) {
                ++load_failures;
                continue;
            }
            feature = analysis::extract_incident_features(
                record.id, record.created_utc_milliseconds, **incident);
            storage::StoredIncidentFeatureCache update{};
            update.incident_id = record.id;
            update.feature_version = feature.version;
            update.values.assign(feature.values.begin(), feature.values.end());
            update.available.reserve(feature.available.size());
            for (const auto available : feature.available)
                update.available.push_back(available ? 1U : 0U);
            updates.push_back(std::move(update));
        }
        inputs.push_back({feature, record.override_group});
    }
    std::string cache_warning;
    if (!updates.empty()) {
        if (const auto stored = recurring_repository_->store_incident_features(updates);
            !stored) {
            cache_warning = "; cache update unavailable: " + stored.error().message;
        }
    }
    const auto clustered = analysis::cluster_incidents(inputs);
    const auto member_row = [&records](const std::int64_t id) {
        const auto record = std::find_if(records->begin(), records->end(),
                                         [id](const auto& value) {
            return value.id == id;
        });
        ui::RecurringIncidentMemberRow row{};
        row.id = id;
        if (record != records->end()) {
            row.created_utc = ui::format_utc_milliseconds(
                record->created_utc_milliseconds);
            row.created_utc_milliseconds = record->created_utc_milliseconds;
            row.label = record->label;
            switch (record->user_feedback) {
            case storage::IncidentUserFeedback::unanswered:
                row.user_feedback = ui::IncidentFeedback::unanswered;
                break;
            case storage::IncidentUserFeedback::noticed_problem:
                row.user_feedback = ui::IncidentFeedback::noticed_problem;
                break;
            case storage::IncidentUserFeedback::did_not_notice_problem:
                row.user_feedback = ui::IncidentFeedback::did_not_notice_problem;
                break;
            }
            switch (record->category) {
            case storage::IncidentCategory::unknown:
                row.category = ui::IncidentCategory::unknown;
                break;
            case storage::IncidentCategory::system_freeze:
                row.category = ui::IncidentCategory::system_freeze;
                break;
            case storage::IncidentCategory::game_stutter:
                row.category = ui::IncidentCategory::game_stutter;
                break;
            case storage::IncidentCategory::application_slowdown_or_hang:
                row.category = ui::IncidentCategory::application_slowdown_or_hang;
                break;
            case storage::IncidentCategory::network:
                row.category = ui::IncidentCategory::network;
                break;
            case storage::IncidentCategory::audio:
                row.category = ui::IncidentCategory::audio;
                break;
            }
        }
        return row;
    };
    ui::RecurringIncidentView view{};
    view.state = ui::RecurringIncidentViewState::ready;
    view.feature_version = clustered.feature_version;
    view.incidents_considered = clustered.inputs_considered;
    view.cached_features = cached_count;
    view.recomputed_features = updates.size();
    view.groups.reserve(clustered.clusters.size());
    for (const auto& cluster : clustered.clusters) {
        ui::RecurringIncidentGroupRow row{};
        row.name = cluster.manually_overridden
            ? "User group: " + cluster.override_group
            : "Automatic pattern " + std::to_string(cluster.stable_key);
        row.manually_overridden = cluster.manually_overridden;
        row.shared_evidence = shared_characteristics_text(cluster);
        row.maximum_pair_distance = cluster.maximum_pair_distance;
        row.shared_characteristic_count = cluster.shared_characteristics.size();
        if (!cluster.shared_characteristics.empty()) {
            for (const auto& characteristic : cluster.shared_characteristics)
                row.average_shared_support += characteristic.support;
            row.average_shared_support /=
                static_cast<double>(cluster.shared_characteristics.size());
        }
        row.members.reserve(cluster.incident_ids.size());
        for (const auto id : cluster.incident_ids)
            row.members.push_back(member_row(id));
        view.groups.push_back(std::move(row));
    }
    view.noise.reserve(clustered.noise_incident_ids.size());
    for (const auto id : clustered.noise_incident_ids)
        view.noise.push_back(member_row(id));
    view.status = std::to_string(view.groups.size()) + " recurring groups from " +
                  std::to_string(view.incidents_considered) + " incidents";
    if (load_failures != 0U)
        view.status += "; " + std::to_string(load_failures) + " load failures";
    view.status += cache_warning;
    view.elapsed_milliseconds = milliseconds(
        std::chrono::steady_clock::now() - started);
    content.recurring = std::move(view);
    if (analyzer_ != nullptr && loaded_incident_ != nullptr &&
        content.detail && content.detail->id == loaded_incident_id_) {
        const auto analysis_started = std::chrono::steady_clock::now();
        const auto recurrence = recurrence_context_for(
            content.recurring, loaded_incident_id_);
        const auto analysis_run = run_analysis(
            analyzer_, profile_repository_, feedback_repository_, loaded_incident_id_,
            *loaded_incident_, recurrence);
        loaded_analysis_ = build_analysis_view(analysis_run, *loaded_incident_);
        content.detail->analysis = loaded_analysis_;
        content.last_analysis_milliseconds = milliseconds(
            std::chrono::steady_clock::now() - analysis_started);
    }
#else
    content.recurring.state = ui::RecurringIncidentViewState::disabled;
    content.recurring.status = "Recurring discovery disabled with analysis";
    content.recurring.elapsed_milliseconds = milliseconds(
        std::chrono::steady_clock::now() - started);
#endif
    publish(std::move(content));
}

void IncidentViewerService::handle_recurring_override(const Job& job) {
    if (recurring_repository_ == nullptr) {
        auto content = *snapshot();
        content.recurring.state = ui::RecurringIncidentViewState::error;
        content.recurring.status = "Recurring storage unavailable";
        publish(std::move(content));
        return;
    }
    const auto updated = recurring_repository_->update_recurring_group_override(
        job.incident_id, job.recurring_group_override);
    if (!updated) {
        auto content = *snapshot();
        content.recurring.state = ui::RecurringIncidentViewState::error;
        content.recurring.status = updated.error().message;
        publish(std::move(content));
        return;
    }
    auto content = *snapshot();
    if (content.detail && content.detail->id == job.incident_id)
        content.detail->recurring_group_override = job.recurring_group_override;
    content.status = job.recurring_group_override.empty()
        ? "Returned incident to automatic recurring grouping"
        : "Recurring group override saved";
    publish(std::move(content));
    handle_recurring();
}

void IncidentViewerService::publish(ui::IncidentViewerContent content) {
    const std::scoped_lock lock{mutex_};
    content.generation = ++generation_;
    snapshot_ = std::make_shared<const ui::IncidentViewerContent>(std::move(content));
}

void IncidentViewerService::publish_error(std::string message) {
    auto content = *snapshot();
    content.state = ui::IncidentViewerLoadState::error;
    content.status = std::move(message);
    publish(std::move(content));
}

} // namespace blackbox::app
