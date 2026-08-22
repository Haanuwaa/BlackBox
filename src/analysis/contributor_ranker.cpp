#include "analysis/contributor_ranker.hpp"

#include "analysis/robust_baseline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>

namespace blackbox::analysis {
namespace {

[[nodiscard]] double process_activity_magnitude(
    const MetricAnomalyEvidence& evidence) noexcept {
    if (evidence.availability != EvidenceAvailability::available ||
        evidence.direction != AnomalyDirection::higher || evidence.score <= 0.0) {
        return 0.0;
    }
    const auto increase = evidence.observed_value - evidence.baseline.median;
    double effect{};
    switch (evidence.metric) {
    case MetricKind::process_cpu:
        if (increase < 0.01) return 0.0;
        effect = increase / 0.25;
        break;
    case MetricKind::process_working_set:
        if (increase < 32.0 * 1024.0 * 1024.0) return 0.0;
        effect = increase / (512.0 * 1024.0 * 1024.0);
        break;
    case MetricKind::process_disk_read:
    case MetricKind::process_disk_write:
        if (increase < 4.0 * 1024.0 * 1024.0) return 0.0;
        effect = increase / (128.0 * 1024.0 * 1024.0);
        break;
    default: return 0.0;
    }
    return (std::min)(evidence.score, std::clamp(effect, 0.0, 1.0));
}

[[nodiscard]] const MetricAnomalyEvidence* strongest_process_evidence(
    const ProcessAnomaly& process) noexcept {
    const MetricAnomalyEvidence* strongest = nullptr;
    double strongest_magnitude{};
    for (const auto& evidence : process.evidence) {
        const auto magnitude = process_activity_magnitude(evidence);
        if (magnitude <= 0.0) continue;
        if (strongest == nullptr || magnitude > strongest_magnitude ||
            (magnitude == strongest_magnitude && evidence.metric < strongest->metric)) {
            strongest = &evidence;
            strongest_magnitude = magnitude;
        }
    }
    return strongest;
}

[[nodiscard]] double cold_start_threshold(const MetricKind metric) noexcept {
    switch (metric) {
    case MetricKind::process_cpu: return 0.10;
    case MetricKind::process_disk_read:
    case MetricKind::process_disk_write: return 16.0 * 1024.0 * 1024.0;
    default: return std::numeric_limits<double>::infinity();
    }
}

[[nodiscard]] double cold_start_score(const MetricKind metric,
                                      const double value) noexcept {
    switch (metric) {
    case MetricKind::process_cpu: return std::clamp(value / 0.50, 0.0, 1.0);
    case MetricKind::process_disk_read:
    case MetricKind::process_disk_write:
        return std::clamp(value / (128.0 * 1024.0 * 1024.0), 0.0, 1.0);
    default: return 0.0;
    }
}

[[nodiscard]] const MetricAnomalyEvidence* strongest_cold_start_evidence(
    const ProcessAnomaly& process) noexcept {
    const MetricAnomalyEvidence* strongest = nullptr;
    double strongest_score{};
    for (const auto& evidence : process.evidence) {
        const auto score = cold_start_process_activity_score(evidence);
        if (score <= 0.0) continue;
        if (strongest == nullptr || score > strongest_score ||
            (score == strongest_score && evidence.metric < strongest->metric)) {
            strongest = &evidence;
            strongest_score = score;
        }
    }
    return strongest;
}

[[nodiscard]] constexpr ResourceKind resource_for(const MetricKind metric) noexcept {
    switch (metric) {
    case MetricKind::process_cpu: return ResourceKind::cpu;
    case MetricKind::process_working_set: return ResourceKind::memory;
    case MetricKind::process_disk_read:
    case MetricKind::process_disk_write: return ResourceKind::disk;
    default: return ResourceKind::cpu;
    }
}

[[nodiscard]] double resource_score(const IncidentAnalysis& analysis,
                                    const ResourceKind resource,
                                    const AnomalyDirection direction) noexcept {
    for (const auto& candidate : analysis.resources) {
        if (candidate.resource != resource) continue;
        double matching_score{};
        for (const auto& evidence : candidate.evidence) {
            if (evidence.availability == EvidenceAvailability::available &&
                evidence.direction == direction) {
                matching_score = (std::max)(matching_score, evidence.score);
            }
        }
        return std::clamp(matching_score, 0.0, 1.0);
    }
    return 0.0;
}

[[nodiscard]] std::optional<double> process_value(
    const core::IncidentProcessSample& sample, const MetricKind metric) noexcept {
    const auto read_double = [](const core::RecordedValue<double>& value)
        -> std::optional<double> {
        if (value.status != core::RecordedValueStatus::available ||
            !std::isfinite(value.value)) return std::nullopt;
        return value.value;
    };
    switch (metric) {
    case MetricKind::process_cpu: return read_double(sample.cpu_fraction);
    case MetricKind::process_working_set:
        if (sample.working_set_bytes.status != core::RecordedValueStatus::available)
            return std::nullopt;
        return static_cast<double>(sample.working_set_bytes.value);
    case MetricKind::process_disk_read:
        return read_double(sample.disk_read_bytes_per_second);
    case MetricKind::process_disk_write:
        return read_double(sample.disk_write_bytes_per_second);
    default: return std::nullopt;
    }
}

[[nodiscard]] bool anomalous(const double value,
                             const MetricAnomalyEvidence& evidence) noexcept {
    const auto z = robust_z_score(evidence.baseline, value);
    if (evidence.direction == AnomalyDirection::higher && z <= 0.0) return false;
    if (evidence.direction == AnomalyDirection::lower && z >= 0.0) return false;
    return anomaly_score(std::abs(z)) > 0.0;
}

struct ActivityWindow {
    std::optional<core::MonotonicTimePoint> first{};
    std::optional<core::MonotonicTimePoint> last{};
    std::size_t anomalous_samples{};
    std::size_t pre_marker_anomalous_samples{};
    std::size_t post_marker_anomalous_samples{};
};

struct LifecycleWindow {
    std::optional<core::MonotonicTimePoint> started{};
    std::optional<core::MonotonicTimePoint> exited{};
};

[[nodiscard]] LifecycleWindow lifecycle_window(
    const core::IncidentSnapshot& incident,
    const core::IncidentProcessIdentity identity,
    const ActivityWindow& activity) noexcept {
    LifecycleWindow result{};
    if (!activity.first || !activity.last) return result;
    for (const auto& event : incident.system_events()) {
        if (event.observed_at < incident.header().actual_start ||
            event.observed_at > incident.header().actual_end ||
            event.source != core::SystemEventSource::process ||
            !event.has_process_identity || event.process_pid != identity.pid ||
            event.process_creation_token != identity.creation_token) {
            continue;
        }
        if (event.kind == core::SystemEventKind::process_started &&
            event.observed_at <= *activity.first &&
            (!result.started || event.observed_at < *result.started)) {
            result.started = event.observed_at;
        } else if (event.kind == core::SystemEventKind::process_exited &&
                   event.observed_at >= *activity.last &&
                   (!result.exited || event.observed_at < *result.exited)) {
            result.exited = event.observed_at;
        }
    }
    return result;
}

[[nodiscard]] double evidence_coverage(const ProcessAnomaly& process,
                                       std::size_t& missing_metrics) noexcept {
    double coverage{};
    missing_metrics = 0U;
    for (const auto& evidence : process.evidence) {
        if (evidence.availability != EvidenceAvailability::available) {
            ++missing_metrics;
            continue;
        }
        const auto total = evidence.evaluation_samples +
                           evidence.missing_evaluation_samples;
        coverage += total == 0U ? 0.0
                                : static_cast<double>(evidence.evaluation_samples) /
                                      static_cast<double>(total);
    }
    return process.evidence.empty()
               ? 0.0
               : coverage / static_cast<double>(process.evidence.size());
}

[[nodiscard]] std::optional<double> history_value(
    const ExecutableProfileObservation& observation,
    const MetricKind metric) noexcept {
    switch (metric) {
    case MetricKind::process_cpu: return observation.cpu_fraction;
    case MetricKind::process_working_set: return observation.working_set_bytes;
    case MetricKind::process_disk_read: return observation.disk_read_bytes_per_second;
    case MetricKind::process_disk_write: return observation.disk_write_bytes_per_second;
    default: return std::nullopt;
    }
}

[[nodiscard]] std::size_t recurrence_count(
    const ProcessAnomaly& process,
    const MetricAnomalyEvidence& evidence,
    const IncidentAnalysisContext& context) noexcept {
    if (process.executable_key.empty()) return 0U;
    std::size_t result{};
    for (const auto& observation : context.executable_history) {
        if (observation.executable_key != process.executable_key ||
            observation.observed_utc_milliseconds > context.observed_utc_milliseconds ||
            (observation.observed_utc_milliseconds == context.observed_utc_milliseconds &&
             observation.incident_id >= context.incident_id)) continue;
        const auto value = history_value(observation, evidence.metric);
        if (value && std::isfinite(*value) && anomalous(*value, evidence)) ++result;
    }
    return result;
}

[[nodiscard]] AnalysisConfidence contributor_confidence(
    const ProcessAnomaly& process, const double coverage,
    const ContributorTemporalRelationship relationship) noexcept {
    if (coverage < 0.5 || process.confidence == AnalysisConfidence::unavailable ||
        process.confidence == AnalysisConfidence::low) return AnalysisConfidence::low;
    if (relationship != ContributorTemporalRelationship::preceding_activity ||
        coverage < 0.75 || process.confidence == AnalysisConfidence::moderate)
        return AnalysisConfidence::moderate;
    return AnalysisConfidence::high;
}

} // namespace

double cold_start_process_activity_score(
    const MetricAnomalyEvidence& evidence) noexcept {
    if (evidence.availability != EvidenceAvailability::insufficient_baseline ||
        evidence.evaluation_samples < 2U ||
        evidence.observed_value < cold_start_threshold(evidence.metric)) {
        return 0.0;
    }
    return cold_start_score(evidence.metric, evidence.observed_value);
}

std::vector<ContributorCandidate> rank_contributors(
    const core::IncidentSnapshot& incident, const IncidentAnalysis& analysis,
    const IncidentAnalysisContext& context, const std::size_t maximum_results) {
    std::vector<ContributorCandidate> result;
    const auto result_limit = (std::min)(maximum_results,
                                         maximum_contributor_candidates);
    if (result_limit == 0U) return result;
    result.reserve((std::min)(result_limit, analysis.processes.size()));
    struct CandidateWork {
        const ProcessAnomaly* process{};
        const MetricAnomalyEvidence* evidence{};
        bool cold_start{};
        ActivityWindow activity{};
    };
    std::vector<CandidateWork> work;
    work.reserve(analysis.processes.size());
    std::map<core::IncidentProcessIdentity, std::size_t> work_by_identity;
    for (const auto& process : analysis.processes) {
        auto* evidence = strongest_process_evidence(process);
        const auto cold_start = evidence == nullptr;
        if (evidence == nullptr) evidence = strongest_cold_start_evidence(process);
        if (evidence == nullptr) continue;
        const auto insertion = work_by_identity.emplace(process.identity,
                                                        work.size());
        if (!insertion.second) continue;
        work.push_back(CandidateWork{&process, evidence, cold_start, {}});
    }
    const auto event = incident.header().window.event_time;
    for (const auto& sample : incident.process_samples()) {
        if (sample.observed_at < analysis.evaluation_start ||
            sample.observed_at > analysis.evaluation_end) continue;
        const auto found = work_by_identity.find(sample.identity);
        if (found == work_by_identity.end()) continue;
        auto& candidate_work = work[found->second];
        const auto value = process_value(sample, candidate_work.evidence->metric);
        if (!value || (candidate_work.cold_start
                           ? *value < cold_start_threshold(candidate_work.evidence->metric)
                           : !anomalous(*value, *candidate_work.evidence))) continue;
        if (!candidate_work.activity.first)
            candidate_work.activity.first = sample.observed_at;
        candidate_work.activity.last = sample.observed_at;
        ++candidate_work.activity.anomalous_samples;
        if (sample.observed_at <= event) {
            ++candidate_work.activity.pre_marker_anomalous_samples;
        } else {
            ++candidate_work.activity.post_marker_anomalous_samples;
        }
    }
    for (const auto& candidate_work : work) {
        const auto& process = *candidate_work.process;
        const auto& evidence = *candidate_work.evidence;
        const auto& activity = candidate_work.activity;
        if (!activity.first || !activity.last || activity.anomalous_samples == 0U) continue;

        ContributorCandidate candidate{};
        candidate.identity = process.identity;
        candidate.name = process.name;
        candidate.executable_key = process.executable_key;
        candidate.strongest_metric = evidence.metric;
        candidate.matched_resource = resource_for(evidence.metric);
        candidate.anomaly_magnitude = candidate_work.cold_start
            ? cold_start_score(evidence.metric, evidence.observed_value)
            : process_activity_magnitude(evidence);
        candidate.resource_match_score = resource_score(
            analysis, candidate.matched_resource, evidence.direction);
        candidate.activity_started_seconds_from_event =
            std::chrono::duration<double>{*activity.first - event}.count();
        const auto lifecycle = lifecycle_window(incident, candidate.identity, activity);
        if (lifecycle.started) {
            candidate.has_process_start_event = true;
            candidate.process_started_seconds_from_event =
                std::chrono::duration<double>{*lifecycle.started - event}.count();
        }
        if (lifecycle.exited) {
            candidate.has_process_exit_event = true;
            candidate.process_exited_seconds_from_event =
                std::chrono::duration<double>{*lifecycle.exited - event}.count();
        }
        candidate.pre_marker_anomalous_samples =
            activity.pre_marker_anomalous_samples;
        candidate.post_marker_anomalous_samples =
            activity.post_marker_anomalous_samples;
        if (candidate.pre_marker_anomalous_samples == 0U) {
            candidate.temporal_relationship =
                ContributorTemporalRelationship::post_marker_reaction;
        } else if (candidate.post_marker_anomalous_samples >
                   candidate.pre_marker_anomalous_samples) {
            candidate.temporal_relationship =
                ContributorTemporalRelationship::marker_spanning_ambiguous;
        } else {
            candidate.temporal_relationship =
                ContributorTemporalRelationship::preceding_activity;
        }
        if (candidate.temporal_relationship ==
            ContributorTemporalRelationship::preceding_activity) {
            const auto lead = -candidate.activity_started_seconds_from_event;
            candidate.timing_score = lead <= 10.0 ? 1.0 : lead <= 30.0 ? 0.8 : 0.5;
        } else if (candidate.temporal_relationship ==
                   ContributorTemporalRelationship::marker_spanning_ambiguous) {
            candidate.timing_score = 0.35;
        } else {
            candidate.timing_score = 0.15;
        }
        candidate.anomalous_duration_seconds = std::max(
            0.0, std::chrono::duration<double>{*activity.last - *activity.first}.count());
        candidate.duration_score = std::min(
            1.0, candidate.anomalous_duration_seconds / 3.0);
        candidate.recurrence_count = recurrence_count(process, evidence, context);
        candidate.recurrence_score = std::min(
            1.0, static_cast<double>(candidate.recurrence_count) / 3.0);
        candidate.evidence_coverage = evidence_coverage(process,
                                                        candidate.missing_metrics);
        if (candidate_work.cold_start) {
            const auto total = evidence.evaluation_samples +
                               evidence.missing_evaluation_samples;
            const auto selected_coverage = total == 0U ? 0.0 :
                static_cast<double>(evidence.evaluation_samples) /
                    static_cast<double>(total);
            candidate.evidence_coverage = 0.75 * selected_coverage;
        }
        const auto raw_score = 0.35 * candidate.anomaly_magnitude +
                               0.25 * candidate.timing_score +
                               0.20 * candidate.resource_match_score +
                               0.15 * candidate.duration_score +
                               0.05 * candidate.recurrence_score;
        candidate.score = std::clamp(
            raw_score * (0.5 + 0.5 * candidate.evidence_coverage), 0.0, 1.0);
        candidate.score_before_feedback = candidate.score;
        candidate.confidence = candidate_work.cold_start
            ? AnalysisConfidence::low
            : contributor_confidence(
                  process, candidate.evidence_coverage,
                  candidate.temporal_relationship);
        candidate.strength = candidate.score >= 0.75 &&
                             candidate.anomaly_magnitude >= 0.75 &&
                             candidate.resource_match_score >= 0.5 &&
                             candidate.temporal_relationship ==
                                 ContributorTemporalRelationship::preceding_activity &&
                             candidate.confidence != AnalysisConfidence::low &&
                             !candidate_work.cold_start
            ? ContributorStrength::likely
            : ContributorStrength::potential;
        result.push_back(std::move(candidate));
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.score != right.score) return left.score > right.score;
        if (left.temporal_relationship != right.temporal_relationship)
            return left.temporal_relationship < right.temporal_relationship;
        return left.identity < right.identity;
    });
    if (result.size() > result_limit) result.resize(result_limit);
    return result;
}

} // namespace blackbox::analysis
