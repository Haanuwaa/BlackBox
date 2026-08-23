#include "evaluation/offline_model_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>
#include <vector>

namespace analysis = blackbox::analysis;
namespace evaluation = blackbox::evaluation;

namespace {

evaluation::DiagnosticEvaluationReport report() {
  evaluation::DiagnosticEvaluationReport result{};
  result.split = evaluation::CorpusSplit::calibration;
  result.truth_rows = 20U;
  result.predictions_matched = 20U;
  result.supported_diagnosis_precision.rate = 0.80;
  result.supported_diagnosis_recall.rate = 0.70;
  result.unknown_truth_abstention.rate = 0.90;
  result.top3_contributor_accuracy.rate = 0.75;
  result.false_assertion_rate.rate = 0.10;
  result.brier_score = 0.15;
  result.expected_calibration_error = 0.08;
  return result;
}

evaluation::OfflineFeatureRow feature_row(const std::string &key) {
  evaluation::OfflineFeatureRow result{};
  result.incident_key = key;
  result.features.version = analysis::incident_feature_version;
  result.features.available[0] = true;
  result.features.values[0] = 0.5;
  return result;
}

} // namespace

TEST_CASE("offline feature export is canonical sorted and label free",
          "[evaluation][offline-ml]") {
  std::vector rows{feature_row("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
                   feature_row("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")};
  const auto serialized = evaluation::serialize_offline_feature_matrix(rows);

  REQUIRE(serialized.has_value());
  CHECK(serialized->starts_with(
      "format_version\t1\nincident_feature_version\t2\nrow_count\t2\n"));
  CHECK(serialized->find("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\t0.5") <
        serialized->find("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\t0.5"));
  CHECK(serialized->find("diagnosis") == std::string::npos);
  CHECK(serialized->find("label") == std::string::npos);
}

TEST_CASE("offline feature export rejects duplicates and invalid values",
          "[evaluation][offline-ml]") {
  const auto duplicate = feature_row("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  CHECK_FALSE(
      evaluation::serialize_offline_feature_matrix({duplicate, duplicate})
          .has_value());

  auto invalid = feature_row("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
  invalid.features.values[0] = std::numeric_limits<double>::infinity();
  CHECK_FALSE(
      evaluation::serialize_offline_feature_matrix({invalid}).has_value());
}

TEST_CASE("offline comparison allows bounded noise and rejects regression",
          "[evaluation][offline-ml]") {
  const auto baseline = report();
  auto candidate = baseline;
  candidate.supported_diagnosis_recall.rate -= 0.01;
  candidate.brier_score += 0.01;
  auto comparison =
      evaluation::compare_offline_model_to_baseline(baseline, candidate);
  REQUIRE(comparison.has_value());
  CHECK(comparison->non_inferior);

  candidate.supported_diagnosis_recall.rate = 0.60;
  comparison =
      evaluation::compare_offline_model_to_baseline(baseline, candidate);
  REQUIRE(comparison.has_value());
  CHECK_FALSE(comparison->non_inferior);
}

TEST_CASE("offline comparison refuses different evaluation populations",
          "[evaluation][offline-ml]") {
  const auto baseline = report();
  auto candidate = baseline;
  candidate.truth_rows += 1U;
  CHECK_FALSE(evaluation::compare_offline_model_to_baseline(baseline, candidate)
                  .has_value());
}
