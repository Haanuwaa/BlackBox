#include "analysis/intelligent_incident_analyzer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace blackbox::analysis {
namespace {

[[nodiscard]] double clamp_unit(const double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] constexpr IncidentType type_for(const ResourceKind resource) noexcept {
    switch (resource) {
    case ResourceKind::cpu: return IncidentType::cpu_pressure;
    case ResourceKind::memory: return IncidentType::memory_pressure;
    case ResourceKind::disk: return IncidentType::storage_pressure;
    case ResourceKind::network: return IncidentType::network_pressure;
    }
    return IncidentType::unknown;
}

[[nodiscard]] const MetricAnomalyEvidence* pressure_evidence(
    const ResourceAnomaly& resource) noexcept {
    if (!resource.pressure_metric) return nullptr;
    const auto found = std::find_if(
        resource.evidence.begin(), resource.evidence.end(), [&](const auto& evidence) {
            return evidence.availability == EvidenceAvailability::available &&
                   evidence.metric == *resource.pressure_metric;
        });
    return found == resource.evidence.end() ? nullptr : &*found;
}

[[nodiscard]] double resource_evidence_coverage(
    const ResourceAnomaly& resource) noexcept {
    const auto* evidence = pressure_evidence(resource);
    if (evidence == nullptr) return 0.0;
    const auto baseline_total = evidence->baseline.sample_count +
                                evidence->missing_baseline_samples;
    const auto evaluation_total = evidence->evaluation_samples +
                                  evidence->missing_evaluation_samples;
    const auto baseline_maturity = clamp_unit(
        static_cast<double>(evidence->baseline.sample_count) / 30.0);
    const auto evaluation_maturity = clamp_unit(
        static_cast<double>(evidence->evaluation_samples) / 3.0);
    const auto baseline_availability = baseline_total == 0U
        ? 0.0
        : static_cast<double>(evidence->baseline.sample_count) /
              static_cast<double>(baseline_total);
    const auto evaluation_availability = evaluation_total == 0U
        ? 0.0
        : static_cast<double>(evidence->evaluation_samples) /
              static_cast<double>(evaluation_total);
    return clamp_unit((baseline_maturity + evaluation_maturity +
                       baseline_availability + evaluation_availability) / 4.0);
}

[[nodiscard]] constexpr bool is_resource_quality_signal(
    const MetricKind metric) noexcept {
    switch (metric) {
    case MetricKind::disk_read_latency:
    case MetricKind::disk_write_latency:
    case MetricKind::disk_service_time:
    case MetricKind::disk_queue_depth:
    case MetricKind::network_connectivity:
    case MetricKind::network_tcp_retransmit:
    case MetricKind::network_interface_changes:
    case MetricKind::network_tcp_failures:
    case MetricKind::network_tcp_resets:
        return true;
    default: return false;
    }
}

[[nodiscard]] bool contributor_aligns_with_symptom(
    const ContributorCandidate& contributor, const ResourceKind resource) noexcept {
    const auto incident_local_score = contributor.score_before_feedback > 0.0
        ? contributor.score_before_feedback
        : contributor.score;
    return contributor.matched_resource == resource &&
           contributor.temporal_relationship ==
               ContributorTemporalRelationship::preceding_activity &&
           incident_local_score >= 0.50 &&
           contributor.anomaly_magnitude >= 0.50 &&
           contributor.resource_match_score >= 0.50;
}

[[nodiscard]] double recurrence_support(
    const IncidentRecurrenceContext& recurrence) noexcept {
    if (!recurrence.available || !recurrence.recurring ||
        recurrence.manually_overridden || recurrence.occurrence_count < 2U) {
        return 0.0;
    }
    const auto occurrences = clamp_unit(
        static_cast<double>(recurrence.occurrence_count - 1U) / 3.0);
    const auto cohesion = 1.0 - clamp_unit(
        recurrence.maximum_pair_distance / 0.20);
    const auto shared = recurrence.shared_characteristic_count == 0U
                            ? 0.0
                            : clamp_unit(recurrence.average_shared_support);
    return clamp_unit(occurrences * (0.5 * cohesion + 0.5 * shared));
}

[[nodiscard]] std::optional<std::size_t> context_probability_index(
    const WorkloadContextAssessment& context) noexcept {
    for (std::size_t index = 0U; index < context.probabilities.size(); ++index) {
        if (context.probabilities[index].context == context.primary) return index;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ResourceKind> automatic_trigger_resource(
    const core::AutomaticIncidentResource resource) noexcept {
    switch (resource) {
    case core::AutomaticIncidentResource::cpu: return ResourceKind::cpu;
    case core::AutomaticIncidentResource::memory: return ResourceKind::memory;
    case core::AutomaticIncidentResource::disk: return ResourceKind::disk;
    case core::AutomaticIncidentResource::network: return ResourceKind::network;
    case core::AutomaticIncidentResource::none: return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] AnalysisConfidence confidence_for(
    const double score, const double coverage, const bool cold_start) noexcept {
    if (score >= 0.75 && coverage >= 0.75 && !cold_start)
        return AnalysisConfidence::high;
    if (score >= 0.50 && coverage >= 0.45)
        return AnalysisConfidence::moderate;
    return AnalysisConfidence::low;
}

[[nodiscard]] constexpr double contributor_confidence_weight(
    const AnalysisConfidence confidence) noexcept {
    switch (confidence) {
    case AnalysisConfidence::unavailable: return 0.0;
    case AnalysisConfidence::low: return 0.50;
    case AnalysisConfidence::moderate: return 0.80;
    case AnalysisConfidence::high: return 1.0;
    }
    return 0.0;
}

class StableFingerprint final {
public:
    void add(const std::uint64_t value) noexcept {
        for (std::size_t shift = 0U; shift < 64U; shift += 8U) {
            hash_ ^= static_cast<std::uint8_t>(value >> shift);
            hash_ *= 1'099'511'628'211ULL;
        }
    }

    [[nodiscard]] std::uint64_t value() const noexcept { return hash_; }

private:
    std::uint64_t hash_{1'469'598'103'934'665'603ULL};
};

} // namespace

std::expected<IntelligentAnalysisConfiguration, IntelligentAnalysisConfigurationError>
validate_intelligent_analysis_configuration(
    const IntelligentAnalysisConfiguration configuration) noexcept {
    if (!validate_personalized_analysis_configuration(configuration.component_analysis)) {
        return std::unexpected{
            IntelligentAnalysisConfigurationError::component_configuration_invalid};
    }
    if (!validate_feedback_calibration_configuration(
            configuration.feedback_calibration)) {
        return std::unexpected{
            IntelligentAnalysisConfigurationError::feedback_configuration_invalid};
    }
    if (!validate_contributor_feedback_calibration_configuration(
            configuration.contributor_feedback_calibration)) {
        return std::unexpected{
            IntelligentAnalysisConfigurationError::
                contributor_feedback_configuration_invalid};
    }
    if (!validate_similar_incident_evidence_configuration(
            configuration.similar_incident_evidence)) {
        return std::unexpected{
            IntelligentAnalysisConfigurationError::similar_incident_configuration_invalid};
    }
    if (!std::isfinite(configuration.minimum_diagnosis_resource_score) ||
        configuration.minimum_diagnosis_resource_score <= 0.0 ||
        configuration.minimum_diagnosis_resource_score > 1.0) {
        return std::unexpected{
            IntelligentAnalysisConfigurationError::diagnosis_threshold_invalid};
    }
    if (!std::isfinite(
            configuration.minimum_feedback_adjusted_assertion_confidence) ||
        configuration.minimum_feedback_adjusted_assertion_confidence <= 0.0 ||
        configuration.minimum_feedback_adjusted_assertion_confidence > 1.0) {
        return std::unexpected{
            IntelligentAnalysisConfigurationError::feedback_assertion_threshold_invalid};
    }
    if (!std::isfinite(configuration.multi_resource_minimum_score) ||
        configuration.multi_resource_minimum_score <= 0.0 ||
        configuration.multi_resource_minimum_score > 1.0) {
        return std::unexpected{
            IntelligentAnalysisConfigurationError::multi_resource_threshold_invalid};
    }
    if (!std::isfinite(configuration.multi_resource_maximum_gap) ||
        configuration.multi_resource_maximum_gap < 0.0 ||
        configuration.multi_resource_maximum_gap > 0.25) {
        return std::unexpected{
            IntelligentAnalysisConfigurationError::multi_resource_gap_invalid};
    }
    if (configuration.maximum_evidence_links == 0U ||
        configuration.maximum_evidence_links > maximum_diagnosis_evidence_links) {
        return std::unexpected{
            IntelligentAnalysisConfigurationError::evidence_limit_invalid};
    }
    return configuration;
}

std::uint64_t intelligent_configuration_fingerprint(
    const IntelligentAnalysisConfiguration& configuration) noexcept {
    StableFingerprint fingerprint;
    fingerprint.add(intelligent_pipeline_version);
    fingerprint.add(diagnosis_evidence_model_version);
    const auto& personalized = configuration.component_analysis;
    const auto& statistical = personalized.incident_local;
    fingerprint.add(static_cast<std::uint64_t>(statistical.baseline_duration.count()));
    fingerprint.add(static_cast<std::uint64_t>(statistical.evaluation_pre_window.count()));
    fingerprint.add(statistical.baseline_capacity);
    fingerprint.add(statistical.minimum_baseline_samples);
    fingerprint.add(statistical.maximum_process_candidates);
    fingerprint.add(statistical.maximum_ranked_processes);
    const auto& floors = statistical.resource_effect_floors;
    fingerprint.add(std::bit_cast<std::uint64_t>(floors.cpu_minimum_value));
    fingerprint.add(std::bit_cast<std::uint64_t>(floors.cpu_minimum_increase));
    fingerprint.add(std::bit_cast<std::uint64_t>(floors.memory_minimum_value));
    fingerprint.add(std::bit_cast<std::uint64_t>(floors.memory_minimum_increase));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        floors.disk_throughput_minimum_bytes_per_second));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        floors.disk_throughput_minimum_increase));
    fingerprint.add(std::bit_cast<std::uint64_t>(floors.disk_latency_minimum_seconds));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        floors.disk_latency_minimum_increase_seconds));
    fingerprint.add(std::bit_cast<std::uint64_t>(floors.disk_queue_minimum_depth));
    fingerprint.add(std::bit_cast<std::uint64_t>(floors.disk_queue_minimum_increase));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        floors.network_throughput_minimum_bytes_per_second));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        floors.network_throughput_minimum_increase));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        floors.network_retransmit_minimum_fraction));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        floors.network_retransmit_minimum_increase));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        floors.network_quality_counter_minimum));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        floors.network_connectivity_minimum_severity));
    fingerprint.add(statistical.workload_context.enabled ? 1U : 0U);
    fingerprint.add(statistical.workload_context.maximum_process_metadata);
    fingerprint.add(statistical.workload_context.maximum_evidence);
    fingerprint.add(std::bit_cast<std::uint64_t>(
        statistical.workload_context.maximum_score_reduction));
    fingerprint.add(static_cast<std::uint64_t>(personalized.maximum_profile_age.count()));
    fingerprint.add(personalized.minimum_profile_observations);
    fingerprint.add(personalized.maximum_profile_observations);
    fingerprint.add(personalized.maximum_profile_updates);
    const auto& feedback = configuration.feedback_calibration;
    fingerprint.add(static_cast<std::uint64_t>(feedback.maximum_age.count()));
    fingerprint.add(feedback.minimum_matching_observations);
    fingerprint.add(feedback.maximum_observations);
    fingerprint.add(std::bit_cast<std::uint64_t>(
        feedback.minimum_false_positive_fraction));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        feedback.maximum_confidence_reduction));
    const auto& contributor_feedback =
        configuration.contributor_feedback_calibration;
    fingerprint.add(static_cast<std::uint64_t>(
        contributor_feedback.maximum_age.count()));
    fingerprint.add(contributor_feedback.minimum_matching_observations);
    fingerprint.add(contributor_feedback.maximum_observations);
    fingerprint.add(std::bit_cast<std::uint64_t>(
        contributor_feedback.minimum_consensus_fraction));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        contributor_feedback.maximum_score_increase));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        contributor_feedback.maximum_score_reduction));
    const auto& similar = configuration.similar_incident_evidence;
    fingerprint.add(static_cast<std::uint64_t>(similar.maximum_age.count()));
    fingerprint.add(similar.minimum_matching_confirmations);
    fingerprint.add(similar.maximum_observations);
    fingerprint.add(std::bit_cast<std::uint64_t>(
        similar.minimum_problem_fraction));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        similar.minimum_category_consensus));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        configuration.minimum_diagnosis_resource_score));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        configuration.minimum_feedback_adjusted_assertion_confidence));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        configuration.multi_resource_minimum_score));
    fingerprint.add(std::bit_cast<std::uint64_t>(
        configuration.multi_resource_maximum_gap));
    fingerprint.add(configuration.maximum_evidence_links);
    return fingerprint.value();
}

IncidentDiagnosis compose_incident_diagnosis(
    const core::IncidentSnapshot& incident, const IncidentAnalysis& analysis,
    const IncidentAnalysisContext& context,
    const IntelligentAnalysisConfiguration& configuration) {
    IncidentDiagnosis diagnosis{};
    // The validated bound is small and known before composition. Reserving it
    // avoids repeated reallocations across the mutually exclusive symptom
    // branches and keeps GCC's optimized vector construction path explicit.
    diagnosis.evidence.reserve(configuration.maximum_evidence_links);
    std::optional<std::size_t> application_crash_event{};
    std::optional<std::size_t> application_hang_event{};
    std::optional<std::size_t> dns_timeout_event{};
    std::optional<std::size_t> display_recovery_event{};
    std::optional<std::size_t> storage_retry_event{};
    for (std::size_t index = 0U; index < incident.system_events().size(); ++index) {
        const auto& event = incident.system_events()[index];
        const auto distance = event.observed_at >= incident.header().window.event_time
            ? event.observed_at - incident.header().window.event_time
            : incident.header().window.event_time - event.observed_at;
        if (event.source == core::SystemEventSource::application &&
            event.kind == core::SystemEventKind::application_crash &&
            event.native_event_id == 1000U &&
            distance <= std::chrono::seconds{5}) {
            application_crash_event = index;
        } else if (event.source == core::SystemEventSource::application &&
            event.kind == core::SystemEventKind::application_hang &&
            event.native_event_id == 1002U &&
            distance <= std::chrono::seconds{5}) {
            application_hang_event = index;
        } else if (event.source == core::SystemEventSource::network &&
                   event.kind == core::SystemEventKind::dns_resolution_timeout &&
                   event.native_event_id == 1014U &&
                   distance <= std::chrono::seconds{5}) {
            dns_timeout_event = index;
        } else if (event.source == core::SystemEventSource::graphics &&
                   event.kind == core::SystemEventKind::display_driver_recovery &&
                   event.native_event_id == 4101U &&
                   distance <= std::chrono::seconds{5}) {
            display_recovery_event = index;
        } else if (event.source == core::SystemEventSource::storage &&
                   event.kind == core::SystemEventKind::storage_io_retry &&
                   event.native_event_id == 153U &&
                   distance <= std::chrono::seconds{5}) {
            storage_retry_event = index;
        }
    }
    const auto automatic_display_recovery =
        incident.header().window.automatic_trigger_count > 0U &&
        incident.header().window.automatic_signal ==
            core::AutomaticIncidentSignal::display_driver_recovery;
    if (automatic_display_recovery || display_recovery_event) {
        diagnosis.available = true;
        diagnosis.type = IncidentType::display_driver_recovery;
        diagnosis.basis = automatic_display_recovery
            ? SymptomExplanationBasis::automatic_capture_alignment
            : SymptomExplanationBasis::system_event_alignment;
        diagnosis.evidence_coverage = 1.0;
        diagnosis.calibrated_confidence = automatic_display_recovery ? 0.99 : 0.96;
        diagnosis.confidence = AnalysisConfidence::high;
        if (display_recovery_event &&
            diagnosis.evidence.size() < configuration.maximum_evidence_links) {
            diagnosis.evidence.push_back({
                DiagnosisEvidenceKind::system_event, *display_recovery_event,
                1.0, automatic_display_recovery ? 0.0 : 0.96});
        }
        if (automatic_display_recovery &&
            diagnosis.evidence.size() < configuration.maximum_evidence_links) {
            diagnosis.evidence.push_back({
                DiagnosisEvidenceKind::automatic_capture_trigger, 0U,
                clamp_unit(incident.header().window.automatic_score), 0.99});
        }
        return diagnosis;
    }
    const auto automatic_storage_retry =
        incident.header().window.automatic_trigger_count > 0U &&
        incident.header().window.automatic_resource ==
            core::AutomaticIncidentResource::disk &&
        incident.header().window.automatic_signal ==
            core::AutomaticIncidentSignal::storage_io_retry;
    if (automatic_storage_retry || storage_retry_event) {
        diagnosis.available = true;
        diagnosis.type = IncidentType::storage_io_retry;
        diagnosis.basis = automatic_storage_retry
            ? SymptomExplanationBasis::automatic_capture_alignment
            : SymptomExplanationBasis::system_event_alignment;
        diagnosis.evidence_coverage = 1.0;
        diagnosis.calibrated_confidence = automatic_storage_retry ? 0.98 : 0.94;
        diagnosis.confidence = AnalysisConfidence::high;
        if (storage_retry_event &&
            diagnosis.evidence.size() < configuration.maximum_evidence_links) {
            diagnosis.evidence.push_back({
                DiagnosisEvidenceKind::system_event, *storage_retry_event,
                1.0, automatic_storage_retry ? 0.0 : 0.94});
        }
        if (automatic_storage_retry &&
            diagnosis.evidence.size() < configuration.maximum_evidence_links) {
            diagnosis.evidence.push_back({
                DiagnosisEvidenceKind::automatic_capture_trigger, 0U,
                clamp_unit(incident.header().window.automatic_score), 0.98});
        }
        return diagnosis;
    }
    const auto automatic_application_crash =
        incident.header().window.automatic_trigger_count > 0U &&
        incident.header().window.automatic_signal ==
            core::AutomaticIncidentSignal::application_crash;
    if (automatic_application_crash || application_crash_event) {
        diagnosis.available = true;
        diagnosis.type = IncidentType::application_crash;
        diagnosis.basis = automatic_application_crash
            ? SymptomExplanationBasis::automatic_capture_alignment
            : SymptomExplanationBasis::system_event_alignment;
        diagnosis.evidence_coverage = 1.0;
        diagnosis.calibrated_confidence = automatic_application_crash ? 0.99 : 0.97;
        diagnosis.confidence = AnalysisConfidence::high;
        if (application_crash_event &&
            diagnosis.evidence.size() < configuration.maximum_evidence_links) {
            diagnosis.evidence.push_back({
                DiagnosisEvidenceKind::system_event, *application_crash_event,
                1.0, automatic_application_crash ? 0.0 : 0.97});
        }
        if (automatic_application_crash &&
            diagnosis.evidence.size() < configuration.maximum_evidence_links) {
            diagnosis.evidence.push_back({
                DiagnosisEvidenceKind::automatic_capture_trigger, 0U,
                clamp_unit(incident.header().window.automatic_score), 0.99});
        }
        return diagnosis;
    }
    const auto automatic_application_hang =
        incident.header().window.automatic_trigger_count > 0U &&
        incident.header().window.automatic_signal ==
            core::AutomaticIncidentSignal::application_hang;
    if (automatic_application_hang || application_hang_event) {
        diagnosis.available = true;
        diagnosis.type = IncidentType::application_hang;
        diagnosis.basis = automatic_application_hang
            ? SymptomExplanationBasis::automatic_capture_alignment
            : SymptomExplanationBasis::system_event_alignment;
        diagnosis.evidence_coverage = 1.0;
        diagnosis.calibrated_confidence = automatic_application_hang ? 0.98 : 0.92;
        diagnosis.confidence = AnalysisConfidence::high;
        if (application_hang_event &&
            diagnosis.evidence.size() < configuration.maximum_evidence_links) {
            diagnosis.evidence.push_back({
                DiagnosisEvidenceKind::system_event, *application_hang_event,
                1.0, automatic_application_hang ? 0.0 : 0.92});
        }
        if (automatic_application_hang &&
            diagnosis.evidence.size() < configuration.maximum_evidence_links) {
            diagnosis.evidence.push_back({
                DiagnosisEvidenceKind::automatic_capture_trigger, 0U,
                clamp_unit(incident.header().window.automatic_score), 0.98});
        }
        return diagnosis;
    }
    const auto dns_timeout_fallback = [&] {
        IncidentDiagnosis fallback{};
        if (!dns_timeout_event) return fallback;
        fallback.available = true;
        fallback.type = IncidentType::dns_resolution_timeout;
        fallback.basis = SymptomExplanationBasis::system_event_alignment;
        fallback.evidence_coverage = 1.0;
        fallback.calibrated_confidence = 0.88;
        fallback.confidence = AnalysisConfidence::high;
        if (fallback.evidence.size() < configuration.maximum_evidence_links) {
            fallback.evidence.push_back({
                DiagnosisEvidenceKind::system_event, *dns_timeout_event,
                1.0, 0.88});
        }
        return fallback;
    };
    if (analysis.resources.empty()) return dns_timeout_fallback();

    std::vector<std::size_t> resource_order(analysis.resources.size());
    for (std::size_t index = 0U; index < resource_order.size(); ++index)
        resource_order[index] = index;
    std::sort(resource_order.begin(), resource_order.end(), [&](const auto left,
                                                                const auto right) {
        const auto& lhs = analysis.resources[left];
        const auto& rhs = analysis.resources[right];
        if (lhs.score != rhs.score) return lhs.score > rhs.score;
        return lhs.resource < rhs.resource;
    });
    auto resource_index = resource_order.front();
    const auto trigger_resource = automatic_trigger_resource(
        incident.header().window.automatic_resource);
    bool trigger_selected_resource = false;
    if (incident.header().window.automatic_trigger_count > 0U &&
        trigger_resource && analysis.resources[resource_index].resource != *trigger_resource) {
        const auto triggered = std::find_if(
            resource_order.begin(), resource_order.end(), [&](const auto index) {
                const auto& candidate = analysis.resources[index];
                return candidate.resource == *trigger_resource &&
                       candidate.score >= configuration.minimum_diagnosis_resource_score &&
                       pressure_evidence(candidate) != nullptr;
            });
        if (triggered != resource_order.end()) {
            resource_index = *triggered;
            trigger_selected_resource = true;
        }
    }
    const auto& resource = analysis.resources[resource_index];
    if (resource.score < configuration.minimum_diagnosis_resource_score ||
        pressure_evidence(resource) == nullptr) {
        return dns_timeout_fallback();
    }

    std::optional<std::size_t> aligned_contributor{};
    for (std::size_t index = 0U; index < analysis.contributors.size(); ++index) {
        if (contributor_aligns_with_symptom(analysis.contributors[index],
                                            resource.resource)) {
            aligned_contributor = index;
            break;
        }
    }

    const auto trigger_matches_resource =
        incident.header().window.automatic_trigger_count > 0U && trigger_resource &&
        *trigger_resource == resource.resource;
    const auto quality_signal = is_resource_quality_signal(*resource.pressure_metric);
    if (!trigger_matches_resource && !quality_signal && !aligned_contributor) {
        // Resource pressure remains inspectable in analysis.resources. It is not
        // promoted to a symptom explanation without independent alignment.
        return dns_timeout_fallback();
    }

    diagnosis.available = true;
    diagnosis.type = type_for(resource.resource);
    diagnosis.basis = trigger_matches_resource
                          ? SymptomExplanationBasis::automatic_capture_alignment
                          : quality_signal
                                ? SymptomExplanationBasis::resource_quality_signal
                                : SymptomExplanationBasis::preceding_contributor_alignment;
    if (!trigger_selected_resource && resource_order.size() > 1U) {
        const auto& second = analysis.resources[resource_order[1U]];
        const auto second_has_quality_signal = second.pressure_metric &&
            is_resource_quality_signal(*second.pressure_metric);
        const auto second_has_contributor = std::any_of(
            analysis.contributors.begin(), analysis.contributors.end(),
            [&](const auto& contributor) {
                return contributor_aligns_with_symptom(contributor, second.resource);
            });
        if (resource.score >= configuration.multi_resource_minimum_score &&
            second.score >= configuration.multi_resource_minimum_score &&
            resource.score - second.score <= configuration.multi_resource_maximum_gap &&
            (second_has_quality_signal || second_has_contributor)) {
            diagnosis.type = IncidentType::multi_resource_pressure;
        }
    }
    diagnosis.evidence_coverage = resource_evidence_coverage(resource);
    const auto add_link = [&](const DiagnosisEvidenceKind kind,
                              const std::size_t source_index,
                              const double source_score,
                              const double contribution) {
        if (diagnosis.evidence.size() == configuration.maximum_evidence_links) return;
        diagnosis.evidence.push_back({kind, source_index, clamp_unit(source_score),
                                      clamp_unit(contribution)});
    };

    const auto resource_contribution = 0.68 * resource.score;
    add_link(DiagnosisEvidenceKind::resource_anomaly, resource_index,
             resource.score, resource_contribution);

    double contributor_signal{};
    if (aligned_contributor) {
        const auto index = *aligned_contributor;
        const auto& contributor = analysis.contributors[index];
        diagnosis.primary_contributor_index = index;
        contributor_signal = contributor.score *
                             contributor_confidence_weight(contributor.confidence);
        add_link(DiagnosisEvidenceKind::contributor_correlation, index,
                 contributor.score, 0.14 * contributor_signal);
        const auto process = std::find_if(
            analysis.processes.begin(), analysis.processes.end(),
            [&](const auto& candidate) {
                return candidate.identity == contributor.identity;
            });
        if (process != analysis.processes.end()) {
            add_link(DiagnosisEvidenceKind::process_anomaly,
                     static_cast<std::size_t>(process - analysis.processes.begin()),
                     process->score, 0.0);
        }
    }

    double context_signal{};
    if (analysis.workload_context.enabled &&
        analysis.workload_context.primary != WorkloadContextKind::unknown) {
        context_signal = clamp_unit(analysis.workload_context.confidence);
        if (const auto index = context_probability_index(analysis.workload_context)) {
            add_link(DiagnosisEvidenceKind::workload_context, *index,
                     context_signal, 0.02 * context_signal);
        }
    }
    const auto recurring_signal = recurrence_support(context.recurrence);
    if (recurring_signal > 0.0) {
        add_link(DiagnosisEvidenceKind::recurring_pattern, 0U,
                 recurring_signal, 0.02 * recurring_signal);
    }
    double trigger_signal{};
    const auto trigger_matches_diagnosis = trigger_resource &&
        (*trigger_resource == resource.resource ||
         (diagnosis.type == IncidentType::multi_resource_pressure &&
          std::any_of(analysis.resources.begin(), analysis.resources.end(),
                      [&](const auto& candidate) {
              return candidate.resource == *trigger_resource &&
                     candidate.score >= configuration.multi_resource_minimum_score;
          })));
    if (incident.header().window.automatic_trigger_count > 0U &&
        trigger_matches_diagnosis) {
        trigger_signal = clamp_unit(incident.header().window.automatic_score);
        add_link(DiagnosisEvidenceKind::automatic_capture_trigger, 0U,
                 trigger_signal, 0.02 * trigger_signal);
    }

    const auto coverage_contribution = 0.12 * diagnosis.evidence_coverage;
    diagnosis.correlated_evidence_penalty =
        contributor_signal > 0.0
            ? 0.08 * (std::min)(resource.score, contributor_signal)
            : 0.0;
    const auto combined = resource_contribution + 0.14 * contributor_signal +
                          coverage_contribution + 0.02 * context_signal +
                          0.02 * recurring_signal + 0.02 * trigger_signal -
                          diagnosis.correlated_evidence_penalty;
    diagnosis.calibrated_confidence = clamp_unit(
        combined * (0.65 + 0.35 * diagnosis.evidence_coverage));
    diagnosis.confidence = confidence_for(
        diagnosis.calibrated_confidence, diagnosis.evidence_coverage,
        analysis.cold_start);
    return diagnosis;
}

bool diagnosis_evidence_links_valid(
    const core::IncidentSnapshot& incident,
    const IncidentAnalysis& analysis) noexcept {
    if (analysis.diagnosis.evidence.size() > maximum_diagnosis_evidence_links)
        return false;
    if (!std::isfinite(analysis.diagnosis.calibrated_confidence) ||
        !std::isfinite(analysis.diagnosis.evidence_coverage) ||
        !std::isfinite(analysis.diagnosis.correlated_evidence_penalty) ||
        !std::isfinite(analysis.diagnosis.confidence_before_feedback) ||
        !std::isfinite(analysis.diagnosis.feedback_multiplier) ||
        analysis.diagnosis.calibrated_confidence < 0.0 ||
        analysis.diagnosis.calibrated_confidence > 1.0 ||
        analysis.diagnosis.evidence_coverage < 0.0 ||
        analysis.diagnosis.evidence_coverage > 1.0 ||
        analysis.diagnosis.correlated_evidence_penalty < 0.0 ||
        analysis.diagnosis.correlated_evidence_penalty > 1.0 ||
        analysis.diagnosis.confidence_before_feedback < 0.0 ||
        analysis.diagnosis.confidence_before_feedback > 1.0 ||
        analysis.diagnosis.feedback_multiplier <= 0.0 ||
        analysis.diagnosis.feedback_multiplier > 1.0) {
        return false;
    }
    if (analysis.diagnosis.suppressed_by_feedback &&
        (analysis.diagnosis.available ||
         analysis.feedback_calibration.state !=
             FeedbackCalibrationState::suppressing)) {
        return false;
    }
    if (analysis.diagnosis.available &&
        (analysis.diagnosis.type == IncidentType::unknown ||
         analysis.diagnosis.basis == SymptomExplanationBasis::none ||
         analysis.diagnosis.evidence.empty())) {
        return false;
    }
    if (!analysis.diagnosis.available &&
        analysis.diagnosis.basis != SymptomExplanationBasis::none) return false;
    if (analysis.diagnosis.available) {
        const auto has_evidence_kind = [&](const DiagnosisEvidenceKind kind) {
            return std::any_of(analysis.diagnosis.evidence.begin(),
                               analysis.diagnosis.evidence.end(),
                               [kind](const auto& link) { return link.kind == kind; });
        };
        if (analysis.diagnosis.basis == SymptomExplanationBasis::system_event_alignment) {
            if (!has_evidence_kind(DiagnosisEvidenceKind::system_event)) return false;
        } else if (analysis.diagnosis.basis ==
                   SymptomExplanationBasis::automatic_capture_alignment) {
            if (!has_evidence_kind(DiagnosisEvidenceKind::automatic_capture_trigger)) {
                return false;
            }
        } else if (!has_evidence_kind(DiagnosisEvidenceKind::resource_anomaly)) {
            return false;
        }
    }
    if (analysis.diagnosis.primary_contributor_index &&
        *analysis.diagnosis.primary_contributor_index >= analysis.contributors.size()) {
        return false;
    }
    for (const auto& link : analysis.diagnosis.evidence) {
        if (!std::isfinite(link.source_score) || !std::isfinite(link.confidence_contribution) ||
            link.source_score < 0.0 || link.source_score > 1.0 ||
            link.confidence_contribution < 0.0 || link.confidence_contribution > 1.0) {
            return false;
        }
        switch (link.kind) {
        case DiagnosisEvidenceKind::resource_anomaly:
            if (link.source_index >= analysis.resources.size()) return false;
            break;
        case DiagnosisEvidenceKind::process_anomaly:
            if (link.source_index >= analysis.processes.size()) return false;
            break;
        case DiagnosisEvidenceKind::contributor_correlation:
            if (link.source_index >= analysis.contributors.size()) return false;
            break;
        case DiagnosisEvidenceKind::workload_context:
            if (link.source_index >= analysis.workload_context.probabilities.size()) return false;
            break;
        case DiagnosisEvidenceKind::recurring_pattern:
            if (!analysis.recurrence.available || !analysis.recurrence.recurring ||
                analysis.recurrence.manually_overridden) return false;
            break;
        case DiagnosisEvidenceKind::automatic_capture_trigger:
            if (incident.header().window.automatic_trigger_count == 0U) return false;
            break;
        case DiagnosisEvidenceKind::system_event:
            if (link.source_index >= incident.system_events().size()) return false;
            break;
        }
    }
    return true;
}

IntelligentIncidentAnalyzer::IntelligentIncidentAnalyzer(
    const IntelligentAnalysisConfiguration configuration)
    : configuration_{configuration}, components_{configuration.component_analysis} {
    if (!validate_intelligent_analysis_configuration(configuration)) {
        throw std::invalid_argument{"invalid intelligent analysis configuration"};
    }
}

std::expected<IncidentAnalysis, AnalysisError> IntelligentIncidentAnalyzer::analyze(
    const core::IncidentSnapshot& incident) const noexcept {
    return analyze(incident, IncidentAnalysisContext{});
}

std::expected<IncidentAnalysis, AnalysisError> IntelligentIncidentAnalyzer::analyze(
    const core::IncidentSnapshot& incident,
    const IncidentAnalysisContext& context) const noexcept {
    auto result = components_.analyze(incident, context);
    if (!result) return result;
    try {
        result->provenance.pipeline_version = intelligent_pipeline_version;
        result->provenance.evidence_model_version = diagnosis_evidence_model_version;
        result->provenance.configuration_fingerprint =
            intelligent_configuration_fingerprint(configuration_);
        result->provenance.native_inference = NativeInferenceStatus::not_adopted;
        result->recurrence = context.recurrence;
        result->similar_incident_evidence = evaluate_similar_incident_evidence(
            context.recurrence.recurring,
            context.recurrence.manually_overridden,
            context.incident_id,
            context.observed_utc_milliseconds,
            context.feedback_profile_reset_after_utc_milliseconds,
            context.similar_incident_history,
            configuration_.similar_incident_evidence);
        calibrate_contributors_from_feedback(
            result->contributors, context.incident_id,
            context.observed_utc_milliseconds,
            context.feedback_profile_reset_after_utc_milliseconds,
            context.contributor_feedback_history,
            configuration_.contributor_feedback_calibration);
        result->diagnosis = compose_incident_diagnosis(
            incident, *result, context, configuration_);
        result->feedback_calibration = calibrate_automatic_trigger_feedback(
            incident, context.incident_id, context.observed_utc_milliseconds,
            context.feedback_history, configuration_.feedback_calibration);
        result->feedback_calibration.profile_revision =
            context.feedback_profile_revision;
        result->feedback_calibration.reset_after_utc_milliseconds =
            context.feedback_profile_reset_after_utc_milliseconds;
        result->feedback_calibration.rollback_available =
            context.feedback_profile_rollback_available;
        result->diagnosis.confidence_before_feedback =
            result->diagnosis.calibrated_confidence;
        if (result->feedback_calibration.state ==
                FeedbackCalibrationState::suppressing &&
            result->diagnosis.available &&
            result->diagnosis.basis ==
                SymptomExplanationBasis::automatic_capture_alignment) {
            result->diagnosis.feedback_multiplier =
                result->feedback_calibration.confidence_multiplier;
            result->diagnosis.calibrated_confidence = clamp_unit(
                result->diagnosis.calibrated_confidence *
                result->diagnosis.feedback_multiplier);
            result->diagnosis.confidence = confidence_for(
                result->diagnosis.calibrated_confidence,
                result->diagnosis.evidence_coverage, result->cold_start);
            if (result->diagnosis.calibrated_confidence <
                configuration_.minimum_feedback_adjusted_assertion_confidence) {
                result->diagnosis.available = false;
                result->diagnosis.type = IncidentType::unknown;
                result->diagnosis.basis = SymptomExplanationBasis::none;
                result->diagnosis.confidence = AnalysisConfidence::unavailable;
                result->diagnosis.primary_contributor_index.reset();
                result->diagnosis.suppressed_by_feedback = true;
            }
        }
        if (!diagnosis_evidence_links_valid(incident, *result)) {
            return std::unexpected{AnalysisError{
                AnalysisErrorCode::internal_error,
                "diagnosis evidence link validation failed"}};
        }
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected{AnalysisError{
            AnalysisErrorCode::out_of_memory,
            "insufficient memory for bounded intelligent analysis"}};
    } catch (...) {
        return std::unexpected{AnalysisError{
            AnalysisErrorCode::internal_error,
            "unexpected intelligent analysis failure"}};
    }
}

bool IntelligentIncidentAnalyzer::uses_personalized_history() const noexcept {
    return true;
}

std::uint32_t IntelligentIncidentAnalyzer::pipeline_version() const noexcept {
    return intelligent_pipeline_version;
}

const IntelligentAnalysisConfiguration&
IntelligentIncidentAnalyzer::configuration() const noexcept {
    return configuration_;
}

} // namespace blackbox::analysis
