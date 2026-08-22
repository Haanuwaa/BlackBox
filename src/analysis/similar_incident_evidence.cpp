#include "analysis/similar_incident_evidence.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace blackbox::analysis {
namespace {

[[nodiscard]] constexpr std::size_t symptom_index(
    const SimilarIncidentSymptom symptom) noexcept {
    return static_cast<std::size_t>(symptom);
}

} // namespace

std::expected<SimilarIncidentEvidenceConfiguration,
              SimilarIncidentEvidenceConfigurationError>
validate_similar_incident_evidence_configuration(
    const SimilarIncidentEvidenceConfiguration configuration) noexcept {
    if (configuration.maximum_age <= std::chrono::milliseconds::zero()) {
        return std::unexpected{
            SimilarIncidentEvidenceConfigurationError::age_not_positive};
    }
    if (configuration.minimum_matching_confirmations < 2U ||
        configuration.maximum_observations <
            configuration.minimum_matching_confirmations ||
        configuration.maximum_observations >
            maximum_similar_incident_feedback_observations) {
        return std::unexpected{
            SimilarIncidentEvidenceConfigurationError::observation_bounds_invalid};
    }
    if (!std::isfinite(configuration.minimum_problem_fraction) ||
        configuration.minimum_problem_fraction <= 0.5 ||
        configuration.minimum_problem_fraction > 1.0) {
        return std::unexpected{
            SimilarIncidentEvidenceConfigurationError::problem_fraction_invalid};
    }
    if (!std::isfinite(configuration.minimum_category_consensus) ||
        configuration.minimum_category_consensus <= 0.5 ||
        configuration.minimum_category_consensus > 1.0) {
        return std::unexpected{
            SimilarIncidentEvidenceConfigurationError::category_consensus_invalid};
    }
    return configuration;
}

ConfirmedSimilarIncidentEvidence evaluate_similar_incident_evidence(
    const bool recurring,
    const bool manually_overridden,
    const std::int64_t incident_id,
    const std::int64_t incident_utc_milliseconds,
    const std::int64_t reset_after_utc_milliseconds,
    const std::span<const SimilarIncidentFeedbackObservation> history,
    const SimilarIncidentEvidenceConfiguration& configuration) {
    ConfirmedSimilarIncidentEvidence result{};
    if (!recurring) return result;
    if (manually_overridden) {
        result.state = SimilarIncidentEvidenceState::manual_group_excluded;
        return result;
    }
    result.state = SimilarIncidentEvidenceState::cold_start;
    if (!validate_similar_incident_evidence_configuration(configuration) ||
        incident_id <= 0 || incident_utc_milliseconds <= 0) {
        return result;
    }

    std::vector<SimilarIncidentFeedbackObservation> eligible;
    eligible.reserve(std::min(history.size(), configuration.maximum_observations));
    const auto maximum_age = configuration.maximum_age.count();
    for (const auto& observation : history) {
        if (observation.incident_id <= 0 ||
            observation.incident_id == incident_id ||
            observation.observed_utc_milliseconds <=
                reset_after_utc_milliseconds ||
            observation.symptom > SimilarIncidentSymptom::audio ||
            observation.feedback > SimilarIncidentFeedback::false_positive) {
            continue;
        }
        const auto strictly_prior =
            observation.observed_utc_milliseconds < incident_utc_milliseconds ||
            (observation.observed_utc_milliseconds == incident_utc_milliseconds &&
             observation.incident_id < incident_id);
        if (!strictly_prior) continue;
        const auto age = incident_utc_milliseconds -
                         observation.observed_utc_milliseconds;
        if (age < 0 || age > maximum_age) continue;
        const auto same_incident = std::find_if(
            eligible.begin(), eligible.end(), [&](const auto& existing) {
                return existing.incident_id == observation.incident_id;
            });
        if (same_incident != eligible.end()) {
            if (observation.observed_utc_milliseconds >
                same_incident->observed_utc_milliseconds) {
                *same_incident = observation;
            }
            continue;
        }
        if (eligible.size() < configuration.maximum_observations) {
            eligible.push_back(observation);
            continue;
        }
        const auto oldest = std::min_element(
            eligible.begin(), eligible.end(), [](const auto& left, const auto& right) {
                if (left.observed_utc_milliseconds !=
                    right.observed_utc_milliseconds) {
                    return left.observed_utc_milliseconds <
                           right.observed_utc_milliseconds;
                }
                return left.incident_id < right.incident_id;
            });
        if (oldest != eligible.end() &&
            (observation.observed_utc_milliseconds >
                 oldest->observed_utc_milliseconds ||
             (observation.observed_utc_milliseconds ==
                  oldest->observed_utc_milliseconds &&
              observation.incident_id > oldest->incident_id))) {
            *oldest = observation;
        }
    }
    std::ranges::sort(eligible, [](const auto& left, const auto& right) {
        if (left.observed_utc_milliseconds != right.observed_utc_milliseconds) {
            return left.observed_utc_milliseconds >
                   right.observed_utc_milliseconds;
        }
        return left.incident_id > right.incident_id;
    });

    std::array<std::size_t, symptom_index(SimilarIncidentSymptom::audio) + 1U>
        category_counts{};
    for (const auto& observation : eligible) {
        ++result.observations_considered;
        if (observation.feedback == SimilarIncidentFeedback::unanswered) continue;
        ++result.answered_observations;
        if (observation.feedback == SimilarIncidentFeedback::false_positive) {
            ++result.false_positive_observations;
            continue;
        }
        ++result.confirmed_problem_observations;
        if (observation.symptom == SimilarIncidentSymptom::unknown) continue;
        ++result.categorized_confirmations;
        ++category_counts[symptom_index(observation.symptom)];
    }

    if (result.answered_observations != 0U) {
        result.problem_fraction =
            static_cast<double>(result.confirmed_problem_observations) /
            static_cast<double>(result.answered_observations);
    }
    const auto dominant = std::max_element(
        std::next(category_counts.begin()), category_counts.end());
    if (dominant != category_counts.end()) {
        result.matching_confirmations = *dominant;
        result.symptom = static_cast<SimilarIncidentSymptom>(
            std::distance(category_counts.begin(), dominant));
    }
    if (result.categorized_confirmations != 0U) {
        result.category_consensus =
            static_cast<double>(result.matching_confirmations) /
            static_cast<double>(result.categorized_confirmations);
    }

    if (result.matching_confirmations <
        configuration.minimum_matching_confirmations) {
        return result;
    }
    if (result.problem_fraction < configuration.minimum_problem_fraction ||
        result.category_consensus < configuration.minimum_category_consensus) {
        result.state = SimilarIncidentEvidenceState::conflicting;
        return result;
    }
    result.state = SimilarIncidentEvidenceState::ready;
    return result;
}

} // namespace blackbox::analysis
