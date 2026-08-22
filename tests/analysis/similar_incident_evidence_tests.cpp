#include "analysis/similar_incident_evidence.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <vector>

namespace analysis = blackbox::analysis;
using namespace std::chrono_literals;

TEST_CASE("confirmed automatic recurrence becomes bounded historical context",
          "[analysis][similar-incidents][feedback]") {
    const std::vector<analysis::SimilarIncidentFeedbackObservation> history{
        {7, 9'700, analysis::SimilarIncidentSymptom::game_stutter,
         analysis::SimilarIncidentFeedback::problem_confirmed},
        {8, 9'800, analysis::SimilarIncidentSymptom::game_stutter,
         analysis::SimilarIncidentFeedback::problem_confirmed},
        {9, 9'900, analysis::SimilarIncidentSymptom::game_stutter,
         analysis::SimilarIncidentFeedback::problem_confirmed},
        {6, 9'600, analysis::SimilarIncidentSymptom::system_freeze,
         analysis::SimilarIncidentFeedback::problem_confirmed},
    };

    const auto result = analysis::evaluate_similar_incident_evidence(
        true, false, 10, 10'000, 0, history);

    CHECK(result.state == analysis::SimilarIncidentEvidenceState::ready);
    CHECK(result.symptom == analysis::SimilarIncidentSymptom::game_stutter);
    CHECK(result.matching_confirmations == 3U);
    CHECK(result.categorized_confirmations == 4U);
    CHECK(result.problem_fraction == 1.0);
    CHECK(result.category_consensus == 0.75);
}

TEST_CASE("manual groups and conflicting recurrence cannot teach analysis",
          "[analysis][similar-incidents][poisoning]") {
    const std::vector<analysis::SimilarIncidentFeedbackObservation> history{
        {1, 9'700, analysis::SimilarIncidentSymptom::network,
         analysis::SimilarIncidentFeedback::problem_confirmed},
        {2, 9'800, analysis::SimilarIncidentSymptom::network,
         analysis::SimilarIncidentFeedback::problem_confirmed},
        {3, 9'900, analysis::SimilarIncidentSymptom::network,
         analysis::SimilarIncidentFeedback::false_positive},
    };

    const auto manual = analysis::evaluate_similar_incident_evidence(
        true, true, 10, 10'000, 0, history);
    CHECK(manual.state ==
          analysis::SimilarIncidentEvidenceState::manual_group_excluded);
    CHECK(manual.observations_considered == 0U);

    const auto conflicting = analysis::evaluate_similar_incident_evidence(
        true, false, 10, 10'000, 0, history);
    CHECK(conflicting.state ==
          analysis::SimilarIncidentEvidenceState::conflicting);
    CHECK(conflicting.problem_fraction == Catch::Approx(2.0 / 3.0));
}

TEST_CASE("similar incident reuse excludes reset stale duplicate current and future rows",
          "[analysis][similar-incidents][bounds]") {
    constexpr auto current = 10'000'000'000LL;
    const auto stale = current -
        std::chrono::duration_cast<std::chrono::milliseconds>(91 * 24h).count();
    const std::vector<analysis::SimilarIncidentFeedbackObservation> history{
        {1, stale, analysis::SimilarIncidentSymptom::audio,
         analysis::SimilarIncidentFeedback::problem_confirmed},
        {2, current - 500, analysis::SimilarIncidentSymptom::audio,
         analysis::SimilarIncidentFeedback::problem_confirmed},
        {2, current - 400, analysis::SimilarIncidentSymptom::audio,
         analysis::SimilarIncidentFeedback::problem_confirmed},
        {3, current - 300, analysis::SimilarIncidentSymptom::audio,
         analysis::SimilarIncidentFeedback::problem_confirmed},
        {10, current, analysis::SimilarIncidentSymptom::audio,
         analysis::SimilarIncidentFeedback::problem_confirmed},
        {11, current + 100, analysis::SimilarIncidentSymptom::audio,
         analysis::SimilarIncidentFeedback::problem_confirmed},
        {4, current - 1'000, analysis::SimilarIncidentSymptom::audio,
         analysis::SimilarIncidentFeedback::problem_confirmed},
    };

    const auto result = analysis::evaluate_similar_incident_evidence(
        true, false, 10, current, current - 900, history);

    CHECK(result.state == analysis::SimilarIncidentEvidenceState::ready);
    CHECK(result.observations_considered == 2U);
    CHECK(result.matching_confirmations == 2U);
}

TEST_CASE("similar incident configuration rejects permissive or unbounded policy",
          "[analysis][similar-incidents][configuration]") {
    auto configuration = analysis::SimilarIncidentEvidenceConfiguration{};
    configuration.minimum_problem_fraction = 0.5;
    CHECK_FALSE(analysis::validate_similar_incident_evidence_configuration(
        configuration));
    configuration = {};
    configuration.maximum_observations =
        analysis::maximum_similar_incident_feedback_observations + 1U;
    CHECK_FALSE(analysis::validate_similar_incident_evidence_configuration(
        configuration));
}
