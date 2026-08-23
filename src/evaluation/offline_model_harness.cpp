#include "evaluation/offline_model_harness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <system_error>

namespace blackbox::evaluation {
namespace {

constexpr std::array<std::string_view,
                     analysis::incident_feature_dimension_count>
    feature_names{"cpu_peak",
                  "cpu_near_marker",
                  "memory_peak",
                  "memory_near_marker",
                  "disk_peak",
                  "disk_near_marker",
                  "network_peak",
                  "network_near_marker",
                  "dominant_pre_marker_share",
                  "dominant_post_marker_share",
                  "duration",
                  "dominant_resource_concentration",
                  "disk_quality_peak",
                  "disk_quality_near_marker",
                  "network_quality_peak",
                  "network_quality_near_marker"};

[[nodiscard]] bool valid_key(const std::string_view key) noexcept {
  if (key.size() != 32U)
    return false;
  return std::all_of(key.begin(), key.end(), [](const char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
  });
}

[[nodiscard]] OfflineHarnessError error(const OfflineHarnessErrorCode code,
                                        const std::string_view message) {
  return {code, std::string{message}};
}

[[nodiscard]] bool valid_rate(const double value) noexcept {
  return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

} // namespace

std::expected<std::string, OfflineHarnessError>
serialize_offline_feature_matrix(std::vector<OfflineFeatureRow> rows) noexcept {
  try {
    if (rows.size() > maximum_offline_feature_rows) {
      return std::unexpected{error(OfflineHarnessErrorCode::limit_exceeded,
                                   "offline feature row limit exceeded")};
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto &left, const auto &right) {
                return left.incident_key < right.incident_key;
              });
    for (std::size_t row = 0U; row < rows.size(); ++row) {
      const auto &current = rows[row];
      if (!valid_key(current.incident_key) ||
          current.features.version != analysis::incident_feature_version) {
        return std::unexpected{error(OfflineHarnessErrorCode::invalid_feature,
                                     "offline feature identity is invalid")};
      }
      if (row != 0U && rows[row - 1U].incident_key == current.incident_key) {
        return std::unexpected{
            error(OfflineHarnessErrorCode::duplicate_incident,
                  "offline feature incident is duplicated")};
      }
      for (std::size_t index = 0U; index < current.features.values.size();
           ++index) {
        if (current.features.available[index] &&
            (!std::isfinite(current.features.values[index]) ||
             current.features.values[index] < 0.0 ||
             current.features.values[index] > 1.0)) {
          return std::unexpected{error(OfflineHarnessErrorCode::invalid_feature,
                                       "offline feature value is invalid")};
        }
      }
    }

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
           << "format_version\t" << offline_feature_matrix_format_version
           << '\n'
           << "incident_feature_version\t" << analysis::incident_feature_version
           << '\n'
           << "row_count\t" << rows.size() << '\n'
           << "incident_key";
    for (const auto name : feature_names)
      output << '\t' << name;
    output << '\n';
    for (const auto &row : rows) {
      output << row.incident_key;
      for (std::size_t index = 0U; index < row.features.values.size();
           ++index) {
        output << '\t';
        if (row.features.available[index])
          output << row.features.values[index];
        else
          output << "NA";
      }
      output << '\n';
    }
    return output.str();
  } catch (const std::exception &exception) {
    return std::unexpected{
        error(OfflineHarnessErrorCode::io, exception.what())};
  } catch (...) {
    return std::unexpected{error(OfflineHarnessErrorCode::io,
                                 "unknown feature serialization failure")};
  }
}

std::expected<void, OfflineHarnessError>
write_offline_feature_matrix(const std::filesystem::path &destination,
                             std::vector<OfflineFeatureRow> rows) noexcept {
  try {
    if (destination.empty() || std::filesystem::exists(destination)) {
      return std::unexpected{error(OfflineHarnessErrorCode::already_exists,
                                   "feature destination must be a new file")};
    }
    auto staging = destination;
    staging += ".partial";
    if (std::filesystem::exists(staging)) {
      return std::unexpected{error(OfflineHarnessErrorCode::already_exists,
                                   "feature staging file already exists")};
    }
    auto serialized = serialize_offline_feature_matrix(std::move(rows));
    if (!serialized)
      return std::unexpected{serialized.error()};
    if (!destination.parent_path().empty()) {
      std::filesystem::create_directories(destination.parent_path());
    }
    {
      std::ofstream output{staging, std::ios::binary | std::ios::trunc};
      if (!output) {
        return std::unexpected{error(OfflineHarnessErrorCode::io,
                                     "cannot create feature staging file")};
      }
      output.write(serialized->data(),
                   static_cast<std::streamsize>(serialized->size()));
      if (!output) {
        return std::unexpected{error(OfflineHarnessErrorCode::io,
                                     "cannot write complete feature matrix")};
      }
    }
    std::filesystem::rename(staging, destination);
    std::ifstream input{destination, std::ios::binary};
    const std::string published{std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{}};
    if (!input || published != *serialized) {
      return std::unexpected{
          error(OfflineHarnessErrorCode::io,
                "published feature matrix failed verification")};
    }
    return {};
  } catch (const std::exception &exception) {
    return std::unexpected{
        error(OfflineHarnessErrorCode::io, exception.what())};
  } catch (...) {
    return std::unexpected{error(OfflineHarnessErrorCode::io,
                                 "unknown feature publication failure")};
  }
}

std::expected<OfflineModelComparison, OfflineHarnessError>
compare_offline_model_to_baseline(
    const DiagnosticEvaluationReport &baseline,
    const DiagnosticEvaluationReport &candidate,
    const OfflineComparisonPolicy policy) noexcept {
  const std::array rates{baseline.supported_diagnosis_precision.rate,
                         baseline.supported_diagnosis_recall.rate,
                         baseline.unknown_truth_abstention.rate,
                         baseline.top3_contributor_accuracy.rate,
                         baseline.false_assertion_rate.rate,
                         candidate.supported_diagnosis_precision.rate,
                         candidate.supported_diagnosis_recall.rate,
                         candidate.unknown_truth_abstention.rate,
                         candidate.top3_contributor_accuracy.rate,
                         candidate.false_assertion_rate.rate};
  if (baseline.split != candidate.split ||
      baseline.truth_rows != candidate.truth_rows ||
      baseline.predictions_missing != candidate.predictions_missing ||
      !std::isfinite(policy.maximum_rate_regression) ||
      policy.maximum_rate_regression < 0.0 ||
      !std::isfinite(policy.maximum_error_increase) ||
      policy.maximum_error_increase < 0.0 ||
      !std::all_of(rates.begin(), rates.end(), valid_rate) ||
      !valid_rate(baseline.brier_score) || !valid_rate(candidate.brier_score) ||
      !valid_rate(baseline.expected_calibration_error) ||
      !valid_rate(candidate.expected_calibration_error)) {
    return std::unexpected{error(OfflineHarnessErrorCode::incomparable_reports,
                                 "offline reports are not comparable")};
  }

  OfflineModelComparison result{};
  result.supported_precision_delta =
      candidate.supported_diagnosis_precision.rate -
      baseline.supported_diagnosis_precision.rate;
  result.supported_recall_delta = candidate.supported_diagnosis_recall.rate -
                                  baseline.supported_diagnosis_recall.rate;
  result.unknown_abstention_delta = candidate.unknown_truth_abstention.rate -
                                    baseline.unknown_truth_abstention.rate;
  result.top3_contributor_delta = candidate.top3_contributor_accuracy.rate -
                                  baseline.top3_contributor_accuracy.rate;
  result.false_assertion_delta =
      candidate.false_assertion_rate.rate - baseline.false_assertion_rate.rate;
  result.brier_score_delta = candidate.brier_score - baseline.brier_score;
  result.calibration_error_delta = candidate.expected_calibration_error -
                                   baseline.expected_calibration_error;
  result.non_inferior =
      result.supported_precision_delta >= -policy.maximum_rate_regression &&
      result.supported_recall_delta >= -policy.maximum_rate_regression &&
      result.unknown_abstention_delta >= -policy.maximum_rate_regression &&
      result.top3_contributor_delta >= -policy.maximum_rate_regression &&
      result.false_assertion_delta <= policy.maximum_error_increase &&
      result.brier_score_delta <= policy.maximum_error_increase &&
      result.calibration_error_delta <= policy.maximum_error_increase;
  return result;
}

} // namespace blackbox::evaluation
