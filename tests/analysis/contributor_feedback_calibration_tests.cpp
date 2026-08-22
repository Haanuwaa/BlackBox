#include "analysis/contributor_feedback_calibration.hpp"
#include "analysis/incident_analyzer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <limits>
#include <vector>

namespace analysis = blackbox::analysis;
using Catch::Approx;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] analysis::ContributorCandidate candidate(
    std::string key = "path:c:/app.exe",
    const analysis::ContributorTemporalRelationship relationship =
        analysis::ContributorTemporalRelationship::preceding_activity) {
    analysis::ContributorCandidate value{};
    value.identity = {42U, 84U};
    value.name = "app.exe";
    value.executable_key = std::move(key);
    value.matched_resource = analysis::ResourceKind::cpu;
    value.temporal_relationship = relationship;
    value.strength = analysis::ContributorStrength::likely;
    value.score = 0.80;
    value.score_before_feedback = value.score;
    return value;
}

[[nodiscard]] analysis::ContributorFeedbackObservation observation(
    const std::int64_t id, const std::int64_t incident_utc,
    const analysis::ContributorFeedbackDisposition disposition,
    std::string key = "path:c:/app.exe",
    const analysis::ResourceKind resource = analysis::ResourceKind::cpu,
    const std::int64_t updated_offset = 1,
    const analysis::ContributorTemporalRelationship relationship =
        analysis::ContributorTemporalRelationship::preceding_activity) {
    return {id, incident_utc, incident_utc + updated_offset, std::move(key),
            resource, disposition, relationship};
}

} // namespace

TEST_CASE("contributor feedback configuration enforces conservative bounds",
          "[analysis][contributor-feedback][configuration]") {
    CHECK(analysis::validate_contributor_feedback_calibration_configuration({}));
    auto configuration = analysis::ContributorFeedbackCalibrationConfiguration{};
    configuration.maximum_age = 0ms;
    CHECK_FALSE(analysis::validate_contributor_feedback_calibration_configuration(
        configuration));
    configuration = {};
    configuration.minimum_matching_observations = 1U;
    CHECK_FALSE(analysis::validate_contributor_feedback_calibration_configuration(
        configuration));
    configuration = {};
    configuration.minimum_consensus_fraction = 0.49;
    CHECK_FALSE(analysis::validate_contributor_feedback_calibration_configuration(
        configuration));
    configuration = {};
    configuration.maximum_score_increase = 0.26;
    CHECK_FALSE(analysis::validate_contributor_feedback_calibration_configuration(
        configuration));
    configuration = {};
    configuration.maximum_score_reduction =
        std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(analysis::validate_contributor_feedback_calibration_configuration(
        configuration));
}

TEST_CASE("explicit exact-match consensus adjusts only existing contributor scores",
          "[analysis][contributor-feedback][calibration]") {
    std::vector<analysis::ContributorFeedbackObservation> confirmed;
    std::vector<analysis::ContributorFeedbackObservation> rejected;
    for (std::int64_t id = 1; id <= 4; ++id) {
        confirmed.push_back(observation(
            id, id * 1'000,
            analysis::ContributorFeedbackDisposition::confirmed_contributor));
        rejected.push_back(observation(
            id, id * 1'000,
            analysis::ContributorFeedbackDisposition::not_a_contributor));
    }
    std::vector<analysis::ContributorCandidate> promoted{candidate()};
    analysis::calibrate_contributors_from_feedback(
        promoted, 10, 10'000, 0, confirmed);
    CHECK(promoted.front().feedback_state ==
          analysis::ContributorFeedbackState::promoted);
    CHECK(promoted.front().score_before_feedback == Approx(0.80));
    CHECK(promoted.front().score > promoted.front().score_before_feedback);
    CHECK(promoted.front().feedback_multiplier > 1.0);
    CHECK(promoted.front().feedback_multiplier <= 1.15);

    std::vector<analysis::ContributorCandidate> reduced{candidate()};
    analysis::calibrate_contributors_from_feedback(
        reduced, 10, 10'000, 0, rejected);
    CHECK(reduced.front().feedback_state ==
          analysis::ContributorFeedbackState::reduced);
    CHECK(reduced.front().score < reduced.front().score_before_feedback);
    CHECK(reduced.front().feedback_multiplier >= 0.70);
}

TEST_CASE("positive attribution never promotes post-marker reactions",
          "[analysis][contributor-feedback][causality]") {
    std::vector<analysis::ContributorFeedbackObservation> history;
    for (std::int64_t id = 1; id <= 4; ++id) {
        history.push_back(observation(
            id, id * 1'000,
            analysis::ContributorFeedbackDisposition::confirmed_contributor));
    }
    std::vector<analysis::ContributorCandidate> candidates{
        candidate("path:c:/app.exe",
                  analysis::ContributorTemporalRelationship::post_marker_reaction)};
    analysis::calibrate_contributors_from_feedback(
        candidates, 10, 10'000, 0, history);
    CHECK(candidates.front().feedback_state ==
          analysis::ContributorFeedbackState::stable);
    CHECK(candidates.front().score == Approx(0.80));
    CHECK(candidates.front().feedback_multiplier == Approx(1.0));
}

TEST_CASE("positive attribution from non-preceding source cannot teach uplift",
          "[analysis][contributor-feedback][causality][poisoning]") {
    std::vector<analysis::ContributorFeedbackObservation> history;
    for (std::int64_t id = 1; id <= 4; ++id) {
        history.push_back(observation(
            id, id * 1'000,
            analysis::ContributorFeedbackDisposition::confirmed_contributor,
            "path:c:/app.exe", analysis::ResourceKind::cpu, 1,
            id % 2 == 0
                ? analysis::ContributorTemporalRelationship::post_marker_reaction
                : analysis::ContributorTemporalRelationship::
                      marker_spanning_ambiguous));
    }
    std::vector<analysis::ContributorCandidate> candidates{candidate()};
    analysis::calibrate_contributors_from_feedback(
        candidates, 10, 10'000, 0, history);
    CHECK(candidates.front().feedback_matching_observations == 0U);
    CHECK(candidates.front().feedback_confirmed_observations == 0U);
    CHECK(candidates.front().feedback_state ==
          analysis::ContributorFeedbackState::cold_start);
    CHECK(candidates.front().score == Approx(0.80));
}

TEST_CASE("contributor feedback excludes poison rows and requires consensus",
          "[analysis][contributor-feedback][poisoning]") {
    std::vector<analysis::ContributorFeedbackObservation> history{
        observation(1, 6'000,
                    analysis::ContributorFeedbackDisposition::confirmed_contributor),
        observation(1, 6'100,
                    analysis::ContributorFeedbackDisposition::confirmed_contributor),
        observation(2, 7'000,
                    analysis::ContributorFeedbackDisposition::confirmed_contributor),
        observation(3, 8'000,
                    analysis::ContributorFeedbackDisposition::not_a_contributor),
        observation(4, 9'000,
                    analysis::ContributorFeedbackDisposition::not_a_contributor),
        observation(10, 10'000,
                    analysis::ContributorFeedbackDisposition::confirmed_contributor),
        observation(11, 11'000,
                    analysis::ContributorFeedbackDisposition::confirmed_contributor),
        observation(5, 7'500,
                    analysis::ContributorFeedbackDisposition::confirmed_contributor,
                    "path:c:/other.exe"),
        observation(6, 7'600,
                    analysis::ContributorFeedbackDisposition::confirmed_contributor,
                    "path:c:/app.exe", analysis::ResourceKind::disk),
        observation(7, 7'700,
                    analysis::ContributorFeedbackDisposition::confirmed_contributor,
                    "path:c:/app.exe", analysis::ResourceKind::cpu, 5'000),
    };
    std::vector<analysis::ContributorCandidate> candidates{candidate()};
    analysis::calibrate_contributors_from_feedback(
        candidates, 10, 10'000, 5'000, history);
    CHECK(candidates.front().feedback_matching_observations == 4U);
    CHECK(candidates.front().feedback_confirmed_observations == 2U);
    CHECK(candidates.front().feedback_rejected_observations == 2U);
    CHECK(candidates.front().feedback_state ==
          analysis::ContributorFeedbackState::conflicting);
    CHECK(candidates.front().score == Approx(0.80));
}

TEST_CASE("contributor feedback scanning is bounded before eligibility filtering",
          "[analysis][contributor-feedback][bounds]") {
    analysis::ContributorFeedbackCalibrationConfiguration configuration{};
    configuration.maximum_observations = 4U;
    configuration.minimum_matching_observations = 4U;
    std::vector<analysis::ContributorFeedbackObservation> history;
    for (std::int64_t id = 1; id <= 100; ++id) {
        history.push_back(observation(
            id, id * 10,
            analysis::ContributorFeedbackDisposition::confirmed_contributor,
            "path:c:/other.exe"));
    }
    for (std::int64_t id = 101; id <= 104; ++id) {
        history.push_back(observation(
            id, id * 10,
            analysis::ContributorFeedbackDisposition::confirmed_contributor));
    }
    std::vector<analysis::ContributorCandidate> candidates{candidate()};
    analysis::calibrate_contributors_from_feedback(
        candidates, 200, 10'000, 0, history, configuration);
    CHECK(candidates.front().feedback_observations_considered == 4U);
    CHECK(candidates.front().feedback_matching_observations == 0U);
    CHECK(candidates.front().feedback_state ==
          analysis::ContributorFeedbackState::cold_start);
}
