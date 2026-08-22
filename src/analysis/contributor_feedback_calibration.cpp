#include "analysis/contributor_feedback_calibration.hpp"

#include "analysis/incident_analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace blackbox::analysis {
namespace {

[[nodiscard]] bool is_prior(const ContributorFeedbackObservation& observation,
                            const std::int64_t incident_id,
                            const std::int64_t incident_utc) noexcept {
    return observation.incident_utc_milliseconds < incident_utc ||
           (observation.incident_utc_milliseconds == incident_utc &&
            observation.incident_id < incident_id);
}

[[nodiscard]] bool eligible(const ContributorFeedbackObservation& observation,
                            const ContributorCandidate& candidate,
                            const std::int64_t incident_id,
                            const std::int64_t incident_utc,
                            const std::int64_t reset_after,
                            const std::int64_t oldest) noexcept {
    return !candidate.executable_key.empty() &&
           observation.incident_id > 0 &&
           observation.incident_id != incident_id &&
           observation.incident_utc_milliseconds >= oldest &&
           observation.incident_utc_milliseconds > reset_after &&
           observation.feedback_updated_utc_milliseconds >=
               observation.incident_utc_milliseconds &&
           observation.feedback_updated_utc_milliseconds <= incident_utc &&
           is_prior(observation, incident_id, incident_utc) &&
           observation.executable_key == candidate.executable_key &&
           observation.resource == candidate.matched_resource &&
           (observation.disposition !=
                ContributorFeedbackDisposition::confirmed_contributor ||
            observation.temporal_relationship ==
                ContributorTemporalRelationship::preceding_activity);
}

} // namespace

bool validate_contributor_feedback_calibration_configuration(
    const ContributorFeedbackCalibrationConfiguration& configuration) noexcept {
    return configuration.maximum_age.count() > 0 &&
           configuration.minimum_matching_observations >= 2U &&
           configuration.maximum_observations >=
               configuration.minimum_matching_observations &&
           configuration.maximum_observations <= 1'024U &&
           std::isfinite(configuration.minimum_consensus_fraction) &&
           configuration.minimum_consensus_fraction >= 0.5 &&
           configuration.minimum_consensus_fraction <= 1.0 &&
           std::isfinite(configuration.maximum_score_increase) &&
           configuration.maximum_score_increase >= 0.0 &&
           configuration.maximum_score_increase <= 0.25 &&
           std::isfinite(configuration.maximum_score_reduction) &&
           configuration.maximum_score_reduction >= 0.0 &&
           configuration.maximum_score_reduction <= 0.50;
}

void calibrate_contributors_from_feedback(
    const std::span<ContributorCandidate> candidates,
    const std::int64_t current_incident_id,
    const std::int64_t current_incident_utc_milliseconds,
    const std::int64_t reset_after_utc_milliseconds,
    const std::span<const ContributorFeedbackObservation> history,
    const ContributorFeedbackCalibrationConfiguration& configuration) {
    if (!validate_contributor_feedback_calibration_configuration(configuration) ||
        current_incident_id <= 0 || current_incident_utc_milliseconds <= 0) {
        return;
    }
    if (history.empty()) return;
    const auto oldest = current_incident_utc_milliseconds <
                                configuration.maximum_age.count()
                            ? 0
                            : current_incident_utc_milliseconds -
                                  configuration.maximum_age.count();
    for (auto& candidate : candidates) {
        candidate.score_before_feedback = candidate.score;
        candidate.feedback_multiplier = 1.0;
        candidate.feedback_state = candidate.executable_key.empty()
            ? ContributorFeedbackState::not_applicable
            : ContributorFeedbackState::cold_start;
        candidate.feedback_observations_considered = 0U;
        candidate.feedback_matching_observations = 0U;
        candidate.feedback_confirmed_observations = 0U;
        candidate.feedback_rejected_observations = 0U;
        candidate.feedback_consensus_fraction = 0.0;
        if (candidate.executable_key.empty()) continue;

        std::set<std::int64_t> counted_incidents;
        for (const auto& observation : history) {
            if (candidate.feedback_observations_considered ==
                configuration.maximum_observations)
                break;
            ++candidate.feedback_observations_considered;
            if (!eligible(observation, candidate, current_incident_id,
                          current_incident_utc_milliseconds,
                          reset_after_utc_milliseconds, oldest)) {
                continue;
            }
            if (!counted_incidents.insert(observation.incident_id).second)
                continue;
            ++candidate.feedback_matching_observations;
            if (observation.disposition ==
                ContributorFeedbackDisposition::confirmed_contributor) {
                ++candidate.feedback_confirmed_observations;
            } else {
                ++candidate.feedback_rejected_observations;
            }
        }
        const auto count = candidate.feedback_matching_observations;
        if (count < configuration.minimum_matching_observations) continue;

        const auto confirmed_fraction =
            static_cast<double>(candidate.feedback_confirmed_observations) /
            static_cast<double>(count);
        const auto rejected_fraction =
            static_cast<double>(candidate.feedback_rejected_observations) /
            static_cast<double>(count);
        candidate.feedback_consensus_fraction =
            (std::max)(confirmed_fraction, rejected_fraction);
        if (candidate.feedback_consensus_fraction <
            configuration.minimum_consensus_fraction) {
            candidate.feedback_state = ContributorFeedbackState::conflicting;
            continue;
        }

        candidate.feedback_state = ContributorFeedbackState::stable;
        // Laplace smoothing prevents a small unanimous set from reaching either cap.
        if (rejected_fraction >= configuration.minimum_consensus_fraction) {
            const auto smoothed_rejected =
                (static_cast<double>(candidate.feedback_rejected_observations) + 1.0) /
                (static_cast<double>(count) + 2.0);
            const auto strength = std::clamp(
                (smoothed_rejected - 0.5) / 0.5, 0.0, 1.0);
            candidate.feedback_multiplier = 1.0 -
                configuration.maximum_score_reduction * strength;
            candidate.feedback_state = ContributorFeedbackState::reduced;
        } else if (confirmed_fraction >= configuration.minimum_consensus_fraction &&
                   candidate.temporal_relationship ==
                       ContributorTemporalRelationship::preceding_activity) {
            const auto smoothed_confirmed =
                (static_cast<double>(candidate.feedback_confirmed_observations) + 1.0) /
                (static_cast<double>(count) + 2.0);
            const auto strength = std::clamp(
                (smoothed_confirmed - 0.5) / 0.5, 0.0, 1.0);
            candidate.feedback_multiplier = 1.0 +
                configuration.maximum_score_increase * strength;
            candidate.feedback_state = ContributorFeedbackState::promoted;
        }
        candidate.score = std::clamp(candidate.score_before_feedback *
                                         candidate.feedback_multiplier,
                                     0.0, 1.0);
        if (candidate.score < 0.75) {
            candidate.strength = ContributorStrength::potential;
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left,
                                                       const auto& right) {
        if (left.score != right.score) return left.score > right.score;
        if (left.score_before_feedback != right.score_before_feedback)
            return left.score_before_feedback > right.score_before_feedback;
        if (left.temporal_relationship != right.temporal_relationship)
            return left.temporal_relationship < right.temporal_relationship;
        return left.identity < right.identity;
    });
}

} // namespace blackbox::analysis
