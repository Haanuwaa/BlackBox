#include "analysis/feedback_calibration.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace blackbox::analysis {

std::expected<FeedbackCalibrationConfiguration,
              FeedbackCalibrationConfigurationError>
validate_feedback_calibration_configuration(
    const FeedbackCalibrationConfiguration configuration) noexcept {
    if (configuration.maximum_age <= std::chrono::milliseconds::zero()) {
        return std::unexpected{
            FeedbackCalibrationConfigurationError::age_not_positive};
    }
    if (configuration.minimum_matching_observations == 0U ||
        configuration.maximum_observations == 0U ||
        configuration.minimum_matching_observations >
            configuration.maximum_observations ||
        configuration.maximum_observations > maximum_feedback_observations) {
        return std::unexpected{
            FeedbackCalibrationConfigurationError::observation_bounds_invalid};
    }
    if (!std::isfinite(configuration.minimum_false_positive_fraction) ||
        configuration.minimum_false_positive_fraction <= 0.5 ||
        configuration.minimum_false_positive_fraction > 1.0) {
        return std::unexpected{
            FeedbackCalibrationConfigurationError::false_positive_fraction_invalid};
    }
    if (!std::isfinite(configuration.maximum_confidence_reduction) ||
        configuration.maximum_confidence_reduction < 0.0 ||
        configuration.maximum_confidence_reduction > 0.75) {
        return std::unexpected{
            FeedbackCalibrationConfigurationError::confidence_reduction_invalid};
    }
    return configuration;
}

FeedbackCalibration calibrate_automatic_trigger_feedback(
    const core::IncidentSnapshot& incident,
    const std::int64_t incident_id,
    const std::int64_t incident_utc_milliseconds,
    const std::span<const FeedbackObservation> history,
    const FeedbackCalibrationConfiguration& configuration) noexcept {
    FeedbackCalibration result{};
    const auto& window = incident.header().window;
    result.automatic_resource = window.automatic_resource;
    result.automatic_signal = window.automatic_signal;
    if (!validate_feedback_calibration_configuration(configuration) ||
        window.automatic_trigger_count == 0U || incident_id <= 0 ||
        incident_utc_milliseconds <= 0) {
        return result;
    }

    result.state = FeedbackCalibrationState::cold_start;
    std::set<std::int64_t> seen_incidents;
    const auto maximum_age = configuration.maximum_age.count();
    const auto limit = (std::min)(history.size(), configuration.maximum_observations);
    for (std::size_t index = 0U; index < limit; ++index) {
        const auto& observation = history[index];
        if (observation.incident_id <= 0 || observation.incident_id == incident_id ||
            observation.observed_utc_milliseconds <= 0 ||
            observation.observed_utc_milliseconds >= incident_utc_milliseconds ||
            incident_utc_milliseconds - observation.observed_utc_milliseconds >
                maximum_age ||
            !seen_incidents.insert(observation.incident_id).second) {
            continue;
        }
        ++result.observations_considered;
        if (observation.automatic_resource != window.automatic_resource ||
            observation.automatic_signal != window.automatic_signal) {
            continue;
        }
        ++result.matching_observations;
        if (observation.disposition == FeedbackDisposition::false_positive) {
            ++result.false_positive_observations;
        } else {
            ++result.confirmed_problem_observations;
        }
    }
    if (result.matching_observations == 0U) return result;

    result.false_positive_fraction =
        static_cast<double>(result.false_positive_observations) /
        static_cast<double>(result.matching_observations);
    // A symmetric Beta(1,1) prior prevents a tiny run of identical answers from
    // appearing certain even after the minimum evidence floor is reached.
    result.smoothed_false_positive_probability =
        static_cast<double>(result.false_positive_observations + 1U) /
        static_cast<double>(result.matching_observations + 2U);
    if (result.matching_observations <
        configuration.minimum_matching_observations) {
        return result;
    }
    result.state = FeedbackCalibrationState::stable;
    if (result.false_positive_fraction <
        configuration.minimum_false_positive_fraction) {
        return result;
    }

    const auto suppression_strength = std::clamp(
        (result.smoothed_false_positive_probability - 0.5) / 0.5, 0.0, 1.0);
    result.confidence_multiplier = std::clamp(
        1.0 - configuration.maximum_confidence_reduction * suppression_strength,
        1.0 - configuration.maximum_confidence_reduction, 1.0);
    result.state = FeedbackCalibrationState::suppressing;
    return result;
}

} // namespace blackbox::analysis
