#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace blackbox::analysis {

enum class ResourceKind : std::uint8_t;
struct ContributorCandidate;
enum class ContributorTemporalRelationship : std::uint8_t;

enum class ContributorFeedbackDisposition : std::uint8_t {
    confirmed_contributor,
    not_a_contributor,
};

struct ContributorFeedbackObservation {
    std::int64_t incident_id{};
    std::int64_t incident_utc_milliseconds{};
    std::int64_t feedback_updated_utc_milliseconds{};
    std::string executable_key{};
    ResourceKind resource{};
    ContributorFeedbackDisposition disposition{
        ContributorFeedbackDisposition::confirmed_contributor};
    ContributorTemporalRelationship temporal_relationship{};
    friend bool operator==(const ContributorFeedbackObservation&,
                           const ContributorFeedbackObservation&) = default;
};

enum class ContributorFeedbackState : std::uint8_t {
    not_applicable,
    cold_start,
    conflicting,
    stable,
    promoted,
    reduced,
};

struct ContributorFeedbackCalibrationConfiguration {
    std::chrono::milliseconds maximum_age{std::chrono::hours{24 * 90}};
    std::size_t minimum_matching_observations{4U};
    std::size_t maximum_observations{256U};
    double minimum_consensus_fraction{0.75};
    double maximum_score_increase{0.15};
    double maximum_score_reduction{0.30};
    friend constexpr bool operator==(
        const ContributorFeedbackCalibrationConfiguration&,
        const ContributorFeedbackCalibrationConfiguration&) = default;
};

[[nodiscard]] bool validate_contributor_feedback_calibration_configuration(
    const ContributorFeedbackCalibrationConfiguration& configuration) noexcept;

// Applies only to already-ranked candidates and retains score_before_feedback.
// Exact executable/resource matches are required. Positive feedback must have
// originated from genuinely preceding activity and never promotes current
// marker-spanning or post-marker activity. Repeated rows for one incident count once.
void calibrate_contributors_from_feedback(
    std::span<ContributorCandidate> candidates,
    std::int64_t current_incident_id,
    std::int64_t current_incident_utc_milliseconds,
    std::int64_t reset_after_utc_milliseconds,
    std::span<const ContributorFeedbackObservation> history,
    const ContributorFeedbackCalibrationConfiguration& configuration = {});

} // namespace blackbox::analysis
