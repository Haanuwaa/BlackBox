#pragma once

#include "core/incident.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace blackbox::analysis {

inline constexpr std::size_t maximum_feedback_observations = 256U;

enum class FeedbackDisposition : std::uint8_t {
    problem_confirmed,
    false_positive,
};

struct FeedbackObservation {
    std::int64_t incident_id{};
    std::int64_t observed_utc_milliseconds{};
    core::AutomaticIncidentResource automatic_resource{
        core::AutomaticIncidentResource::none};
    core::AutomaticIncidentSignal automatic_signal{
        core::AutomaticIncidentSignal::throughput_or_utilization};
    FeedbackDisposition disposition{FeedbackDisposition::problem_confirmed};
    friend bool operator==(const FeedbackObservation&,
                           const FeedbackObservation&) = default;
};

struct FeedbackCalibrationConfiguration {
    std::chrono::milliseconds maximum_age{std::chrono::hours{24 * 90}};
    std::size_t minimum_matching_observations{4U};
    std::size_t maximum_observations{maximum_feedback_observations};
    double minimum_false_positive_fraction{0.75};
    double maximum_confidence_reduction{0.55};
    friend constexpr bool operator==(const FeedbackCalibrationConfiguration&,
                                     const FeedbackCalibrationConfiguration&) = default;
};

enum class FeedbackCalibrationConfigurationError : std::uint8_t {
    age_not_positive,
    observation_bounds_invalid,
    false_positive_fraction_invalid,
    confidence_reduction_invalid,
};

[[nodiscard]] std::expected<FeedbackCalibrationConfiguration,
                            FeedbackCalibrationConfigurationError>
validate_feedback_calibration_configuration(
    FeedbackCalibrationConfiguration configuration) noexcept;

enum class FeedbackCalibrationState : std::uint8_t {
    not_applicable,
    cold_start,
    stable,
    suppressing,
};

struct FeedbackCalibration {
    FeedbackCalibrationState state{FeedbackCalibrationState::not_applicable};
    core::AutomaticIncidentResource automatic_resource{
        core::AutomaticIncidentResource::none};
    core::AutomaticIncidentSignal automatic_signal{
        core::AutomaticIncidentSignal::throughput_or_utilization};
    std::size_t observations_considered{};
    std::size_t matching_observations{};
    std::size_t confirmed_problem_observations{};
    std::size_t false_positive_observations{};
    double false_positive_fraction{};
    double smoothed_false_positive_probability{};
    double confidence_multiplier{1.0};
    std::uint64_t profile_revision{};
    std::int64_t reset_after_utc_milliseconds{};
    bool rollback_available{};
    friend bool operator==(const FeedbackCalibration&,
                           const FeedbackCalibration&) = default;
};

// Calibrates only an automatic trigger's exact resource/signal signature. It never
// changes raw evidence and deliberately ignores the current incident, future rows,
// stale rows, duplicates, and unbounded input beyond the configured prefix.
[[nodiscard]] FeedbackCalibration calibrate_automatic_trigger_feedback(
    const core::IncidentSnapshot& incident,
    std::int64_t incident_id,
    std::int64_t incident_utc_milliseconds,
    std::span<const FeedbackObservation> history,
    const FeedbackCalibrationConfiguration& configuration = {}) noexcept;

} // namespace blackbox::analysis
