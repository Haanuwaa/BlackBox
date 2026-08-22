#include "analysis/feedback_calibration.hpp"
#include "diagnosis_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <limits>
#include <vector>

namespace analysis = blackbox::analysis;
namespace fixture = blackbox::test::diagnosis_fixture;

namespace {

[[nodiscard]] analysis::FeedbackObservation observation(
    const std::int64_t id, const std::int64_t utc,
    const analysis::FeedbackDisposition disposition,
    const blackbox::core::AutomaticIncidentResource resource =
        blackbox::core::AutomaticIncidentResource::cpu,
    const blackbox::core::AutomaticIncidentSignal signal =
        blackbox::core::AutomaticIncidentSignal::throughput_or_utilization) {
    return {id, utc, resource, signal, disposition};
}

} // namespace

TEST_CASE("feedback calibration configuration enforces bounded conservative learning",
          "[analysis][feedback][configuration]") {
    CHECK(analysis::validate_feedback_calibration_configuration({}).has_value());
    auto configuration = analysis::FeedbackCalibrationConfiguration{};
    configuration.maximum_age = std::chrono::milliseconds::zero();
    CHECK_FALSE(analysis::validate_feedback_calibration_configuration(configuration));
    configuration = {};
    configuration.minimum_matching_observations = 0U;
    CHECK_FALSE(analysis::validate_feedback_calibration_configuration(configuration));
    configuration = {};
    configuration.maximum_observations =
        analysis::maximum_feedback_observations + 1U;
    CHECK_FALSE(analysis::validate_feedback_calibration_configuration(configuration));
    configuration = {};
    configuration.minimum_false_positive_fraction = 0.5;
    CHECK_FALSE(analysis::validate_feedback_calibration_configuration(configuration));
    configuration = {};
    configuration.maximum_confidence_reduction =
        std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(analysis::validate_feedback_calibration_configuration(configuration));
}

TEST_CASE("matching repeated false positives conservatively calibrate only future triggers",
          "[analysis][feedback][false-positive][poisoning]") {
    const auto incident = fixture::incident(analysis::ResourceKind::cpu,
                                            std::nullopt, true);
    std::vector<analysis::FeedbackObservation> history{
        observation(1, 1'000, analysis::FeedbackDisposition::false_positive),
        observation(2, 2'000, analysis::FeedbackDisposition::false_positive),
        observation(3, 3'000, analysis::FeedbackDisposition::false_positive),
        observation(4, 4'000, analysis::FeedbackDisposition::false_positive),
    };
    const auto calibrated = analysis::calibrate_automatic_trigger_feedback(
        *incident, 10, 10'000, history);
    CHECK(calibrated.state == analysis::FeedbackCalibrationState::suppressing);
    CHECK(calibrated.observations_considered == 4U);
    CHECK(calibrated.matching_observations == 4U);
    CHECK(calibrated.false_positive_observations == 4U);
    CHECK(calibrated.confirmed_problem_observations == 0U);
    CHECK(calibrated.false_positive_fraction == 1.0);
    CHECK(calibrated.smoothed_false_positive_probability < 1.0);
    CHECK(calibrated.confidence_multiplier < 1.0);
    CHECK(calibrated.confidence_multiplier >= 0.45);
}

TEST_CASE("feedback calibration ignores mismatches duplicates current future and stale rows",
          "[analysis][feedback][bounds][poisoning]") {
    using namespace std::chrono_literals;
    const auto incident = fixture::incident(analysis::ResourceKind::cpu,
                                            std::nullopt, true);
    constexpr std::int64_t day_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(24h).count();
    const std::int64_t current_utc = 100 * day_ms;
    std::vector<analysis::FeedbackObservation> history{
        observation(1, current_utc - 1'000,
                    analysis::FeedbackDisposition::problem_confirmed),
        observation(1, current_utc - 2'000,
                    analysis::FeedbackDisposition::false_positive),
        observation(10, current_utc - 1'000,
                    analysis::FeedbackDisposition::false_positive),
        observation(2, current_utc + 1'000,
                    analysis::FeedbackDisposition::false_positive),
        observation(3, current_utc - 91 * day_ms,
                    analysis::FeedbackDisposition::false_positive),
        observation(4, current_utc - 1'000,
                    analysis::FeedbackDisposition::false_positive,
                    blackbox::core::AutomaticIncidentResource::disk),
        observation(5, current_utc - 1'000,
                    analysis::FeedbackDisposition::false_positive,
                    blackbox::core::AutomaticIncidentResource::cpu,
                    blackbox::core::AutomaticIncidentSignal::disk_latency),
    };
    const auto calibrated = analysis::calibrate_automatic_trigger_feedback(
        *incident, 10, current_utc, history);
    CHECK(calibrated.state == analysis::FeedbackCalibrationState::cold_start);
    CHECK(calibrated.observations_considered == 3U);
    CHECK(calibrated.matching_observations == 1U);
    CHECK(calibrated.confirmed_problem_observations == 1U);
    CHECK(calibrated.confidence_multiplier == 1.0);
}

TEST_CASE("conflicting feedback prevents false positive suppression",
          "[analysis][feedback][noise]") {
    const auto incident = fixture::incident(analysis::ResourceKind::cpu,
                                            std::nullopt, true);
    std::vector<analysis::FeedbackObservation> history{
        observation(1, 1'000, analysis::FeedbackDisposition::false_positive),
        observation(2, 2'000, analysis::FeedbackDisposition::false_positive),
        observation(3, 3'000, analysis::FeedbackDisposition::problem_confirmed),
        observation(4, 4'000, analysis::FeedbackDisposition::problem_confirmed),
    };
    const auto calibrated = analysis::calibrate_automatic_trigger_feedback(
        *incident, 10, 10'000, history);
    CHECK(calibrated.state == analysis::FeedbackCalibrationState::stable);
    CHECK(calibrated.false_positive_fraction == 0.5);
    CHECK(calibrated.confidence_multiplier == 1.0);
}

TEST_CASE("manual incidents never consume automatic feedback profiles",
          "[analysis][feedback][manual]") {
    const auto incident = fixture::incident(analysis::ResourceKind::cpu);
    const std::vector history{
        observation(1, 1'000, analysis::FeedbackDisposition::false_positive),
        observation(2, 2'000, analysis::FeedbackDisposition::false_positive),
        observation(3, 3'000, analysis::FeedbackDisposition::false_positive),
        observation(4, 4'000, analysis::FeedbackDisposition::false_positive),
    };
    const auto calibrated = analysis::calibrate_automatic_trigger_feedback(
        *incident, 10, 10'000, history);
    CHECK(calibrated.state == analysis::FeedbackCalibrationState::not_applicable);
    CHECK(calibrated.observations_considered == 0U);
}
