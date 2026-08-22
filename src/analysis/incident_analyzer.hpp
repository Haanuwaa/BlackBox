#pragma once

#include "analysis/contributor_feedback_calibration.hpp"
#include "analysis/feedback_calibration.hpp"
#include "analysis/similar_incident_evidence.hpp"
#include "core/incident.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace blackbox::analysis {

enum class AnalysisErrorCode : std::uint8_t {
    invalid_configuration,
    invalid_incident,
    out_of_memory,
    internal_error,
};

struct AnalysisError {
    AnalysisErrorCode code{AnalysisErrorCode::internal_error};
    std::string message{};
    friend bool operator==(const AnalysisError&, const AnalysisError&) = default;
};

enum class MetricKind : std::uint8_t {
    system_cpu,
    system_memory,
    disk_read,
    disk_write,
    disk_read_latency,
    disk_write_latency,
    disk_service_time,
    disk_queue_depth,
    network_receive,
    network_transmit,
    network_connectivity,
    network_interface_changes,
    network_tcp_retransmit,
    network_tcp_failures,
    network_tcp_resets,
    process_cpu,
    process_working_set,
    process_disk_read,
    process_disk_write,
};

enum class ResourceKind : std::uint8_t {
    cpu,
    memory,
    disk,
    network,
};

enum class EvidenceAvailability : std::uint8_t {
    available,
    insufficient_baseline,
    unavailable,
};

enum class AnomalyDirection : std::uint8_t {
    unchanged,
    higher,
    lower,
};

enum class AnalysisConfidence : std::uint8_t {
    unavailable,
    low,
    moderate,
    high,
};

enum class BaselineScope : std::uint8_t {
    incident_local,
    personalized_executable,
};

enum class PersonalizationState : std::uint8_t {
    not_applicable,
    cold_start,
    ready,
};

enum class ContributorStrength : std::uint8_t {
    potential,
    likely,
};

enum class ContributorTemporalRelationship : std::uint8_t {
    preceding_activity,
    marker_spanning_ambiguous,
    post_marker_reaction,
};

enum class WorkloadContextKind : std::uint8_t {
    unknown,
    idle,
    gaming,
    development,
    compilation,
    video_playback_or_call,
    heavy_download,
    desktop,
};

enum class ContextSignalKind : std::uint8_t {
    average_cpu,
    average_memory,
    average_disk_throughput,
    average_network_throughput,
    low_activity,
    moderate_activity,
    process_name_match,
    ambiguous_margin,
    system_sample_coverage,
};

struct WorkloadContextProbability {
    WorkloadContextKind context{WorkloadContextKind::unknown};
    double probability{};
    friend bool operator==(const WorkloadContextProbability&,
                           const WorkloadContextProbability&) = default;
};

struct WorkloadContextEvidence {
    WorkloadContextKind context{WorkloadContextKind::unknown};
    ContextSignalKind signal{ContextSignalKind::system_sample_coverage};
    double observed_value{};
    double contribution{};
    friend bool operator==(const WorkloadContextEvidence&,
                           const WorkloadContextEvidence&) = default;
};

struct WorkloadContextAssessment {
    bool enabled{};
    WorkloadContextKind primary{WorkloadContextKind::unknown};
    double confidence{};
    double uncertainty{1.0};
    std::size_t system_samples_considered{};
    std::size_t process_metadata_considered{};
    std::vector<WorkloadContextProbability> probabilities{};
    std::vector<WorkloadContextEvidence> evidence{};
    friend bool operator==(const WorkloadContextAssessment&,
                           const WorkloadContextAssessment&) = default;
};

enum class ExecutableIdentitySource : std::uint8_t {
    normalized_path,
    normalized_name,
};

struct NormalizedExecutableIdentity {
    std::string key{};
    std::string display_name{};
    ExecutableIdentitySource source{ExecutableIdentitySource::normalized_name};
    friend bool operator==(const NormalizedExecutableIdentity&,
                           const NormalizedExecutableIdentity&) = default;
};

struct ExecutableProfileObservation {
    std::string executable_key{};
    std::string display_name{};
    std::int64_t incident_id{};
    std::int64_t observed_utc_milliseconds{};
    std::optional<double> cpu_fraction{};
    std::optional<double> working_set_bytes{};
    std::optional<double> disk_read_bytes_per_second{};
    std::optional<double> disk_write_bytes_per_second{};
    friend bool operator==(const ExecutableProfileObservation&,
                           const ExecutableProfileObservation&) = default;
};

struct IncidentRecurrenceContext {
    bool available{};
    bool recurring{};
    bool manually_overridden{};
    std::size_t occurrence_count{};
    std::size_t shared_characteristic_count{};
    double average_shared_support{};
    double maximum_pair_distance{};
    friend bool operator==(const IncidentRecurrenceContext&,
                           const IncidentRecurrenceContext&) = default;
};

struct IncidentAnalysisContext {
    std::int64_t incident_id{};
    std::int64_t observed_utc_milliseconds{};
    std::span<const ExecutableProfileObservation> executable_history{};
    IncidentRecurrenceContext recurrence{};
    std::span<const FeedbackObservation> feedback_history{};
    std::uint64_t feedback_profile_revision{};
    std::int64_t feedback_profile_reset_after_utc_milliseconds{};
    bool feedback_profile_rollback_available{};
    std::span<const SimilarIncidentFeedbackObservation> similar_incident_history{};
    std::span<const ContributorFeedbackObservation> contributor_feedback_history{};
};

struct RobustBaselineSummary {
    std::size_t sample_count{};
    double minimum{};
    double p05{};
    double p25{};
    double median{};
    double p75{};
    double p95{};
    double maximum{};
    double median_absolute_deviation{};
    double robust_scale{};
    friend bool operator==(const RobustBaselineSummary&,
                           const RobustBaselineSummary&) = default;
};

struct MetricAnomalyEvidence {
    MetricKind metric{MetricKind::system_cpu};
    BaselineScope baseline_scope{BaselineScope::incident_local};
    EvidenceAvailability availability{EvidenceAvailability::unavailable};
    AnomalyDirection direction{AnomalyDirection::unchanged};
    double score{};
    double robust_z{};
    double baseline_percentile{};
    double observed_value{};
    core::MonotonicTimePoint observed_at{};
    RobustBaselineSummary baseline{};
    std::size_t evaluation_samples{};
    std::size_t missing_baseline_samples{};
    std::size_t missing_evaluation_samples{};
    friend bool operator==(const MetricAnomalyEvidence&,
                           const MetricAnomalyEvidence&) = default;
};

struct ResourceAnomaly {
    ResourceKind resource{ResourceKind::cpu};
    double score{};
    // Maximum robust statistical deviation before practical effect gating.
    // A large value with score==0 means unusual but not meaningful pressure.
    double statistical_score{};
    // Metric that established practical resource pressure. Empty means the
    // resource has no practically meaningful pressure, even if unusual.
    std::optional<MetricKind> pressure_metric{};
    AnalysisConfidence confidence{AnalysisConfidence::unavailable};
    std::vector<MetricAnomalyEvidence> evidence{};
    double uncontextualized_score{};
    double context_multiplier{1.0};
    friend bool operator==(const ResourceAnomaly&, const ResourceAnomaly&) = default;
};

struct ProcessAnomaly {
    core::IncidentProcessIdentity identity{};
    std::string name{};
    double score{};
    AnalysisConfidence confidence{AnalysisConfidence::unavailable};
    std::string executable_key{};
    PersonalizationState personalization{PersonalizationState::not_applicable};
    std::size_t personalized_observations{};
    std::vector<MetricAnomalyEvidence> evidence{};
    double uncontextualized_score{};
    double context_multiplier{1.0};
    friend bool operator==(const ProcessAnomaly&, const ProcessAnomaly&) = default;
};

struct ContributorCandidate {
    core::IncidentProcessIdentity identity{};
    std::string name{};
    std::string executable_key{};
    MetricKind strongest_metric{MetricKind::process_cpu};
    ResourceKind matched_resource{ResourceKind::cpu};
    ContributorStrength strength{ContributorStrength::potential};
    ContributorTemporalRelationship temporal_relationship{
        ContributorTemporalRelationship::preceding_activity};
    AnalysisConfidence confidence{AnalysisConfidence::unavailable};
    double score{};
    double score_before_feedback{};
    double feedback_multiplier{1.0};
    ContributorFeedbackState feedback_state{
        ContributorFeedbackState::not_applicable};
    std::size_t feedback_observations_considered{};
    std::size_t feedback_matching_observations{};
    std::size_t feedback_confirmed_observations{};
    std::size_t feedback_rejected_observations{};
    double feedback_consensus_fraction{};
    double anomaly_magnitude{};
    double timing_score{};
    double resource_match_score{};
    double duration_score{};
    double recurrence_score{};
    double evidence_coverage{};
    double activity_started_seconds_from_event{};
    bool has_process_start_event{};
    double process_started_seconds_from_event{};
    bool has_process_exit_event{};
    double process_exited_seconds_from_event{};
    double anomalous_duration_seconds{};
    std::size_t pre_marker_anomalous_samples{};
    std::size_t post_marker_anomalous_samples{};
    std::size_t recurrence_count{};
    std::size_t missing_metrics{};
    friend bool operator==(const ContributorCandidate&,
                           const ContributorCandidate&) = default;
};

enum class IncidentType : std::uint8_t {
    unknown,
    cpu_pressure,
    memory_pressure,
    storage_pressure,
    network_pressure,
    multi_resource_pressure,
    application_crash,
    application_hang,
    dns_resolution_timeout,
    display_driver_recovery,
    storage_io_retry,
};

enum class SymptomExplanationBasis : std::uint8_t {
    none,
    automatic_capture_alignment,
    resource_quality_signal,
    preceding_contributor_alignment,
    system_event_alignment,
};

enum class DiagnosisEvidenceKind : std::uint8_t {
    resource_anomaly,
    process_anomaly,
    contributor_correlation,
    workload_context,
    recurring_pattern,
    automatic_capture_trigger,
    system_event,
};

struct DiagnosisEvidenceLink {
    DiagnosisEvidenceKind kind{DiagnosisEvidenceKind::resource_anomaly};
    std::size_t source_index{};
    double source_score{};
    double confidence_contribution{};
    friend bool operator==(const DiagnosisEvidenceLink&,
                           const DiagnosisEvidenceLink&) = default;
};

struct IncidentDiagnosis {
    bool available{};
    IncidentType type{IncidentType::unknown};
    SymptomExplanationBasis basis{SymptomExplanationBasis::none};
    AnalysisConfidence confidence{AnalysisConfidence::unavailable};
    double calibrated_confidence{};
    double evidence_coverage{};
    double correlated_evidence_penalty{};
    double confidence_before_feedback{};
    double feedback_multiplier{1.0};
    bool suppressed_by_feedback{};
    std::optional<std::size_t> primary_contributor_index{};
    std::vector<DiagnosisEvidenceLink> evidence{};
    friend bool operator==(const IncidentDiagnosis&, const IncidentDiagnosis&) = default;
};

enum class NativeInferenceStatus : std::uint8_t {
    not_adopted,
};

struct AnalysisProvenance {
    std::uint32_t pipeline_version{};
    std::uint32_t evidence_model_version{};
    std::uint64_t configuration_fingerprint{};
    NativeInferenceStatus native_inference{NativeInferenceStatus::not_adopted};
    friend bool operator==(const AnalysisProvenance&, const AnalysisProvenance&) = default;
};

struct IncidentAnalysis {
    bool cold_start{};
    core::MonotonicTimePoint baseline_start{};
    core::MonotonicTimePoint baseline_end{};
    core::MonotonicTimePoint evaluation_start{};
    core::MonotonicTimePoint evaluation_end{};
    std::size_t system_samples_considered{};
    std::size_t process_samples_considered{};
    std::size_t missing_values{};
    AnalysisProvenance provenance{};
    IncidentDiagnosis diagnosis{};
    FeedbackCalibration feedback_calibration{};
    ConfirmedSimilarIncidentEvidence similar_incident_evidence{};
    WorkloadContextAssessment workload_context{};
    IncidentRecurrenceContext recurrence{};
    std::vector<ResourceAnomaly> resources{};
    std::vector<ProcessAnomaly> processes{};
    std::vector<ContributorCandidate> contributors{};
    std::vector<ExecutableProfileObservation> profile_updates{};
    friend bool operator==(const IncidentAnalysis&, const IncidentAnalysis&) = default;
};

class IIncidentAnalyzer {
public:
    virtual ~IIncidentAnalyzer() = default;

    [[nodiscard]] virtual std::expected<IncidentAnalysis, AnalysisError> analyze(
        const core::IncidentSnapshot& incident) const noexcept = 0;
    [[nodiscard]] virtual std::expected<IncidentAnalysis, AnalysisError> analyze(
        const core::IncidentSnapshot& incident,
        const IncidentAnalysisContext& context) const noexcept {
        static_cast<void>(context);
        return analyze(incident);
    }
    [[nodiscard]] virtual bool uses_personalized_history() const noexcept {
        return false;
    }
    [[nodiscard]] virtual std::uint32_t pipeline_version() const noexcept {
        return 0U;
    }
};

[[nodiscard]] std::optional<NormalizedExecutableIdentity>
normalize_executable_identity(const core::IncidentProcessInfo& process);

} // namespace blackbox::analysis
