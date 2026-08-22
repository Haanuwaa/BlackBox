#include "evaluation/diagnostic_evaluation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace analysis = blackbox::analysis;
namespace evaluation = blackbox::evaluation;
using Catch::Approx;

namespace {

[[nodiscard]] evaluation::IncidentTruth truth(
    const char key, const analysis::IncidentType diagnosis,
    const std::optional<std::size_t> contributor,
    const std::string& family = {}) {
    return {std::string(31U, '0') + key, "held", evaluation::CorpusSplit::held_out,
            evaluation::SymptomClass::cpu_starvation,
            evaluation::TruthCertainty::confirmed, true, diagnosis, contributor,
            analysis::WorkloadContextKind::development, family, true,
            evaluation::UsefulnessRating::useful, 1U, false};
}

[[nodiscard]] evaluation::DogfoodCorpus corpus() {
    evaluation::DogfoodCorpus value{};
    value.manifest = {"metrics-v1", true, 2U, 123U, 456U};
    value.hardware_profiles.push_back(
        {"host", "windows", "win11", "x64", 8U, "16-31", "unknown", "balanced"});
    value.sessions.push_back({"held", "host", "operator-a", evaluation::CorpusSplit::held_out,
                              evaluation::DogfoodSessionKind::natural,
                              evaluation::SymptomClass::cpu_starvation,
                              600.0, 4U, 0U, true});
    value.sessions.push_back({"quiet", "host", "operator-a", evaluation::CorpusSplit::held_out,
                              evaluation::DogfoodSessionKind::quiet,
                              evaluation::SymptomClass::quiet,
                              7'200.0, 0U, 1U, true});
    value.incidents = {
        truth('1', analysis::IncidentType::cpu_pressure, 2U, "family-a"),
        truth('2', analysis::IncidentType::cpu_pressure, 3U, "family-a"),
        truth('3', analysis::IncidentType::storage_pressure, std::nullopt),
        truth('4', analysis::IncidentType::network_pressure, std::nullopt),
    };
    return value;
}

} // namespace

TEST_CASE("held-out evaluator reports predeclared accuracy calibration and exposure metrics",
          "[evaluation][metrics]") {
    const auto input = corpus();
    const std::vector<evaluation::DiagnosticPrediction> predictions{
        {std::string(31U, '0') + '1', analysis::IncidentType::cpu_pressure, 0.8,
         {2U, 7U}, analysis::WorkloadContextKind::development, true, "cluster-a"},
        {std::string(31U, '0') + '2', analysis::IncidentType::cpu_pressure, 0.6,
         {9U, 3U}, analysis::WorkloadContextKind::development, false, "cluster-a"},
        {std::string(31U, '0') + '3', analysis::IncidentType::network_pressure, 0.7,
         {}, analysis::WorkloadContextKind::desktop, true, {}},
        {std::string(31U, '0') + '4', analysis::IncidentType::unknown, 0.0,
         {}, analysis::WorkloadContextKind::development, true, {}},
    };
    auto report = evaluation::evaluate_diagnostics(input, predictions);
    REQUIRE(report.has_value());
    CHECK(report->supported_diagnosis_recall.eligible == 4U);
    CHECK(report->supported_diagnosis_recall.successful == 2U);
    CHECK(report->supported_diagnosis_recall.rate == Approx(0.5));
    CHECK(report->supported_diagnosis_precision.eligible == 3U);
    CHECK(report->supported_diagnosis_precision.successful == 2U);
    CHECK(report->supported_diagnosis_precision.rate == Approx(2.0 / 3.0));
    CHECK(report->unknown_truth_abstention.eligible == 0U);
    CHECK(report->false_assertion_rate.rate == Approx(0.0));
    CHECK(report->top1_contributor_accuracy.rate == Approx(0.5));
    CHECK(report->top3_contributor_accuracy.rate == Approx(1.0));
    CHECK(report->context_accuracy.rate == Approx(0.75));
    CHECK(report->automatic_detection_recall.rate == Approx(0.75));
    CHECK(report->automatic_detection_miss_rate.rate == Approx(0.25));
    CHECK(report->usefulness.rate == Approx(1.0));
    CHECK(report->unknown_rate.eligible == 4U);
    CHECK(report->unknown_rate.successful == 1U);
    CHECK(report->unknown_rate.rate == Approx(0.25));
    CHECK(report->quiet_automatic_captures == 1U);
    CHECK(report->quiet_exposure_hours == Approx(2.0));
    CHECK(report->false_captures_per_hour == Approx(0.5));
    CHECK(report->calibration_rows == 3U);
    CHECK(report->brier_score == Approx((0.04 + 0.16 + 0.49) / 3.0));
    CHECK(report->recurrence_true_pairs == 1U);
    CHECK(report->recurrence_predicted_pairs == 1U);
    CHECK(report->recurrence_true_positive_pairs == 1U);
    CHECK(report->recurrence_pair_f1 == Approx(1.0));
}

TEST_CASE("V0.15.1 qualification requires every predeclared held-out floor",
          "[evaluation][metrics][qualification]") {
    evaluation::DiagnosticEvaluationReport report{};
    report.supported_diagnosis_precision = {10U, 8U, 0.80};
    report.supported_diagnosis_recall = {10U, 6U, 0.60};
    report.unknown_truth_abstention = {10U, 9U, 0.90};
    report.top3_contributor_accuracy = {10U, 7U, 0.70};
    CHECK(evaluation::qualify_v0151(report).passed);
    report.top3_contributor_accuracy.rate = 0.69;
    CHECK_FALSE(evaluation::qualify_v0151(report).passed);
    report.top3_contributor_accuracy = {};
    CHECK_FALSE(evaluation::qualify_v0151(report).passed);
}

TEST_CASE("quiet and unresolved truth penalize confident false assertions",
          "[evaluation][metrics]") {
    auto input = corpus();
    input.incidents[0].expected_diagnosis = analysis::IncidentType::unknown;
    input.incidents[1].expected_diagnosis = analysis::IncidentType::unknown;
    const std::vector<evaluation::DiagnosticPrediction> predictions{
        {input.incidents[0].incident_key, analysis::IncidentType::cpu_pressure, 0.9},
        {input.incidents[1].incident_key, analysis::IncidentType::unknown, 0.0},
        {input.incidents[2].incident_key, analysis::IncidentType::storage_pressure, 0.8},
        {input.incidents[3].incident_key, analysis::IncidentType::network_pressure, 0.8},
    };
    auto report = evaluation::evaluate_diagnostics(input, predictions);
    REQUIRE(report.has_value());
    CHECK(report->unknown_truth_abstention.eligible == 2U);
    CHECK(report->unknown_truth_abstention.successful == 1U);
    CHECK(report->false_assertion_rate.eligible == 2U);
    CHECK(report->false_assertion_rate.successful == 1U);
    CHECK(report->false_assertion_rate.rate == Approx(0.5));
    CHECK(report->brier_score == Approx((0.81 + 0.04 + 0.04) / 3.0));
}

TEST_CASE("uncertain and disputed truth cannot inflate primary metrics",
          "[evaluation][metrics]") {
    auto input = corpus();
    input.incidents[0].certainty = evaluation::TruthCertainty::uncertain;
    input.incidents[1].disagreement = true;
    const std::vector<evaluation::DiagnosticPrediction> predictions{
        {input.incidents[0].incident_key, analysis::IncidentType::cpu_pressure, 1.0},
        {input.incidents[1].incident_key, analysis::IncidentType::cpu_pressure, 1.0},
        {input.incidents[2].incident_key, analysis::IncidentType::storage_pressure, 1.0},
        {input.incidents[3].incident_key, analysis::IncidentType::network_pressure, 1.0},
    };
    auto report = evaluation::evaluate_diagnostics(input, predictions);
    REQUIRE(report.has_value());
    CHECK(report->uncertain_excluded == 1U);
    CHECK(report->disagreement_excluded == 1U);
    CHECK(report->supported_diagnosis_recall.eligible == 2U);
    CHECK(report->supported_diagnosis_recall.rate == Approx(1.0));
}

TEST_CASE("missing predictions are failures in every truth-based metric",
          "[evaluation][metrics][qualification]") {
    auto input = corpus();
    input.incidents[3].expected_diagnosis = analysis::IncidentType::unknown;
    const std::vector<evaluation::DiagnosticPrediction> predictions{
        {input.incidents[0].incident_key, analysis::IncidentType::cpu_pressure, 0.9,
         {2U}, analysis::WorkloadContextKind::development, true, "cluster-a"},
    };

    const auto report = evaluation::evaluate_diagnostics(input, predictions);
    REQUIRE(report.has_value());
    CHECK(report->predictions_matched == 1U);
    CHECK(report->predictions_missing == 3U);
    CHECK(report->supported_diagnosis_recall.eligible == 3U);
    CHECK(report->supported_diagnosis_recall.successful == 1U);
    CHECK(report->unknown_truth_abstention.eligible == 1U);
    CHECK(report->unknown_truth_abstention.successful == 0U);
    CHECK(report->top3_contributor_accuracy.eligible == 2U);
    CHECK(report->top3_contributor_accuracy.successful == 1U);
    CHECK(report->context_accuracy.eligible == 4U);
    CHECK(report->automatic_detection_recall.eligible == 4U);
    CHECK(report->automatic_detection_miss_rate.eligible == 4U);
    CHECK(report->automatic_detection_miss_rate.successful == 3U);
    CHECK(report->unknown_rate.eligible == 4U);
    CHECK(report->unknown_rate.successful == 0U);
    CHECK(report->usefulness.eligible == 4U);
    CHECK(report->recurrence_true_pairs == 1U);
    CHECK(report->recurrence_predicted_pairs == 0U);
    CHECK_FALSE(evaluation::qualify_v0151(*report).passed);
}

TEST_CASE("evaluator rejects unfrozen duplicate and unpaired predictions",
          "[evaluation][metrics]") {
    auto input = corpus();
    input.manifest.frozen = false;
    REQUIRE_FALSE(evaluation::evaluate_diagnostics(input, {}).has_value());
    input.manifest.frozen = true;
    evaluation::DiagnosticPrediction prediction{
        input.incidents.front().incident_key, analysis::IncidentType::cpu_pressure, 0.5};
    auto duplicate = evaluation::evaluate_diagnostics(input, {prediction, prediction});
    REQUIRE_FALSE(duplicate.has_value());
    prediction.incident_key = std::string(32U, 'f');
    auto unpaired = evaluation::evaluate_diagnostics(input, {prediction});
    REQUIRE_FALSE(unpaired.has_value());

    prediction.incident_key = input.incidents.front().incident_key;
    prediction.contributor_ordinals.push_back(8'192U);
    CHECK_FALSE(evaluation::evaluate_diagnostics(input, {prediction}).has_value());
    prediction.contributor_ordinals.clear();
    prediction.recurrence_cluster = "invalid\tcluster";
    CHECK_FALSE(evaluation::evaluate_diagnostics(input, {prediction}).has_value());
    prediction.recurrence_cluster.clear();
    prediction.practical_pressure_score = 1.01;
    CHECK_FALSE(evaluation::evaluate_diagnostics(input, {prediction}).has_value());
}

TEST_CASE("isotonic calibration is bounded monotonic and refuses tiny samples",
          "[evaluation][calibration]") {
    CHECK_FALSE(evaluation::fit_isotonic_confidence_calibration(
        std::vector<evaluation::CalibrationSample>(9U, {0.5, true})).has_value());
    std::vector<evaluation::CalibrationSample> samples;
    for (std::size_t index = 0U; index < 20U; ++index) {
        samples.push_back({static_cast<double>(index) / 20.0, index >= 12U});
    }
    auto model = evaluation::fit_isotonic_confidence_calibration(samples);
    REQUIRE(model.has_value());
    REQUIRE_FALSE(model->knots.empty());
    CHECK(model->source_samples == 20U);
    for (std::size_t index = 1U; index < model->knots.size(); ++index) {
        CHECK(model->knots[index - 1U].maximum_input <=
              model->knots[index].maximum_input);
        CHECK(model->knots[index - 1U].calibrated_probability <=
              model->knots[index].calibrated_probability);
    }
    CHECK(evaluation::apply_confidence_calibration(*model, -1.0) >= 0.0);
    CHECK(evaluation::apply_confidence_calibration(*model, 2.0) <= 1.0);
}

TEST_CASE("assertion threshold maximizes coverage without weakening precision",
          "[evaluation][calibration]") {
    const std::vector<evaluation::CalibrationSample> samples{
        {0.95, true}, {0.90, true}, {0.85, false}, {0.80, true},
        {0.70, false}, {0.60, false}};
    const auto selected = evaluation::select_assertion_threshold(samples, 0.75);
    CHECK(selected.assertions_enabled);
    CHECK(selected.threshold == Approx(0.80));
    CHECK(selected.asserted_rows == 4U);
    CHECK(selected.observed_precision == Approx(0.75));

    const auto refused = evaluation::select_assertion_threshold(
        {{0.9, false}, {0.8, false}}, 0.80);
    CHECK_FALSE(refused.assertions_enabled);
}
