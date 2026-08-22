#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace blackbox::analysis {

inline constexpr std::size_t maximum_similar_incident_feedback_observations = 32U;

enum class SimilarIncidentSymptom : std::uint8_t {
    unknown,
    system_freeze,
    game_stutter,
    application_slowdown_or_hang,
    network,
    audio,
};

enum class SimilarIncidentFeedback : std::uint8_t {
    unanswered,
    problem_confirmed,
    false_positive,
};

struct SimilarIncidentFeedbackObservation {
    std::int64_t incident_id{};
    std::int64_t observed_utc_milliseconds{};
    SimilarIncidentSymptom symptom{SimilarIncidentSymptom::unknown};
    SimilarIncidentFeedback feedback{SimilarIncidentFeedback::unanswered};
    friend bool operator==(const SimilarIncidentFeedbackObservation&,
                           const SimilarIncidentFeedbackObservation&) = default;
};

struct SimilarIncidentEvidenceConfiguration {
    std::chrono::milliseconds maximum_age{std::chrono::hours{24 * 90}};
    std::size_t minimum_matching_confirmations{2U};
    std::size_t maximum_observations{maximum_similar_incident_feedback_observations};
    double minimum_problem_fraction{0.75};
    double minimum_category_consensus{0.75};
    friend constexpr bool operator==(const SimilarIncidentEvidenceConfiguration&,
                                     const SimilarIncidentEvidenceConfiguration&) = default;
};

enum class SimilarIncidentEvidenceConfigurationError : std::uint8_t {
    age_not_positive,
    observation_bounds_invalid,
    problem_fraction_invalid,
    category_consensus_invalid,
};

[[nodiscard]] std::expected<SimilarIncidentEvidenceConfiguration,
                            SimilarIncidentEvidenceConfigurationError>
validate_similar_incident_evidence_configuration(
    SimilarIncidentEvidenceConfiguration configuration) noexcept;

enum class SimilarIncidentEvidenceState : std::uint8_t {
    not_applicable,
    manual_group_excluded,
    cold_start,
    conflicting,
    ready,
};

struct ConfirmedSimilarIncidentEvidence {
    SimilarIncidentEvidenceState state{SimilarIncidentEvidenceState::not_applicable};
    SimilarIncidentSymptom symptom{SimilarIncidentSymptom::unknown};
    std::size_t observations_considered{};
    std::size_t answered_observations{};
    std::size_t confirmed_problem_observations{};
    std::size_t false_positive_observations{};
    std::size_t categorized_confirmations{};
    std::size_t matching_confirmations{};
    double problem_fraction{};
    double category_consensus{};
    friend bool operator==(const ConfirmedSimilarIncidentEvidence&,
                           const ConfirmedSimilarIncidentEvidence&) = default;
};

// Reuses only bounded, prior feedback from an automatic recurrence cluster.
// The returned summary is historical context. It is not causal evidence and
// must not change current-incident scores, rankings, or recorded telemetry.
[[nodiscard]] ConfirmedSimilarIncidentEvidence evaluate_similar_incident_evidence(
    bool recurring,
    bool manually_overridden,
    std::int64_t incident_id,
    std::int64_t incident_utc_milliseconds,
    std::int64_t reset_after_utc_milliseconds,
    std::span<const SimilarIncidentFeedbackObservation> history,
    const SimilarIncidentEvidenceConfiguration& configuration = {});

} // namespace blackbox::analysis
