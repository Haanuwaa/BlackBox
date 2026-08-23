#pragma once

#include "analysis/incident_clustering.hpp"
#include "evaluation/diagnostic_evaluation.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace blackbox::evaluation {

inline constexpr std::uint32_t offline_feature_matrix_format_version = 1U;
inline constexpr std::size_t maximum_offline_feature_rows = 10'000U;

struct OfflineFeatureRow {
  std::string incident_key{};
  analysis::IncidentFeatureVector features{};
  friend bool operator==(const OfflineFeatureRow &,
                         const OfflineFeatureRow &) = default;
};

enum class OfflineHarnessErrorCode : std::uint8_t {
  io,
  already_exists,
  invalid_feature,
  duplicate_incident,
  limit_exceeded,
  incomparable_reports,
};

struct OfflineHarnessError {
  OfflineHarnessErrorCode code{OfflineHarnessErrorCode::invalid_feature};
  std::string message{};
  friend bool operator==(const OfflineHarnessError &,
                         const OfflineHarnessError &) = default;
};

[[nodiscard]] std::expected<std::string, OfflineHarnessError>
serialize_offline_feature_matrix(std::vector<OfflineFeatureRow> rows) noexcept;

// Publishes one new sibling-staged, label-free direct-V1 feature matrix.
// Incident IDs, timestamps, process identities, paths, and truth labels are
// deliberately absent so model work stays downstream of immutable evidence.
[[nodiscard]] std::expected<void, OfflineHarnessError>
write_offline_feature_matrix(const std::filesystem::path &destination,
                             std::vector<OfflineFeatureRow> rows) noexcept;

struct OfflineComparisonPolicy {
  double maximum_rate_regression{0.02};
  double maximum_error_increase{0.02};
  friend bool operator==(const OfflineComparisonPolicy &,
                         const OfflineComparisonPolicy &) = default;
};

struct OfflineModelComparison {
  bool non_inferior{};
  double supported_precision_delta{};
  double supported_recall_delta{};
  double unknown_abstention_delta{};
  double top3_contributor_delta{};
  double false_assertion_delta{};
  double brier_score_delta{};
  double calibration_error_delta{};
  friend bool operator==(const OfflineModelComparison &,
                         const OfflineModelComparison &) = default;
};

// Compares reports only after their canonical prediction artifacts have been
// independently verified by the caller against the same frozen corpus.
[[nodiscard]] std::expected<OfflineModelComparison, OfflineHarnessError>
compare_offline_model_to_baseline(const DiagnosticEvaluationReport &baseline,
                                  const DiagnosticEvaluationReport &candidate,
                                  OfflineComparisonPolicy policy = {}) noexcept;

} // namespace blackbox::evaluation
