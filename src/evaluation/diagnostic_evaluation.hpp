#pragma once

#include "evaluation/dogfood_corpus.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace blackbox::evaluation {

inline constexpr std::size_t calibration_bin_count = 10U;
inline constexpr std::size_t maximum_calibration_knots = 32U;
inline constexpr std::uint32_t diagnostic_evaluation_report_format_version = 1U;
inline constexpr std::uint32_t confidence_calibration_artifact_format_version = 1U;

struct DiagnosticPrediction {
    std::string incident_key{};
    analysis::IncidentType diagnosis{analysis::IncidentType::unknown};
    double confidence{};
    std::vector<std::size_t> contributor_ordinals{};
    analysis::WorkloadContextKind context{analysis::WorkloadContextKind::unknown};
    bool automatic_capture{};
    std::string recurrence_cluster{};
    std::optional<analysis::ResourceKind> observed_pressure{};
    double practical_pressure_score{};
    double raw_statistical_score{};
    friend bool operator==(const DiagnosticPrediction&,
                           const DiagnosticPrediction&) = default;
};

struct RateMetric {
    std::size_t eligible{};
    std::size_t successful{};
    double rate{};
    friend bool operator==(const RateMetric&, const RateMetric&) = default;
};

struct CalibrationBin {
    std::size_t count{};
    double average_confidence{};
    double accuracy{};
    friend bool operator==(const CalibrationBin&, const CalibrationBin&) = default;
};

struct DiagnosticEvaluationReport {
    CorpusSplit split{CorpusSplit::held_out};
    std::size_t truth_rows{};
    std::size_t predictions_matched{};
    std::size_t predictions_missing{};
    std::size_t uncertain_excluded{};
    std::size_t disagreement_excluded{};
    std::array<std::size_t, dogfood_symptom_class_count> symptom_counts{};
    std::size_t hardware_profiles_represented{};
    RateMetric supported_diagnosis_recall{};
    RateMetric supported_diagnosis_precision{};
    RateMetric unknown_truth_abstention{};
    RateMetric top1_contributor_accuracy{};
    RateMetric top3_contributor_accuracy{};
    RateMetric context_accuracy{};
    RateMetric automatic_detection_recall{};
    RateMetric automatic_detection_miss_rate{};
    RateMetric usefulness{};
    RateMetric unknown_rate{};
    RateMetric false_assertion_rate{};
    std::size_t quiet_automatic_captures{};
    double quiet_exposure_hours{};
    double false_captures_per_hour{};
    std::size_t calibration_rows{};
    double brier_score{};
    double expected_calibration_error{};
    std::array<CalibrationBin, calibration_bin_count> calibration_bins{};
    std::size_t recurrence_true_pairs{};
    std::size_t recurrence_predicted_pairs{};
    std::size_t recurrence_true_positive_pairs{};
    double recurrence_pair_precision{};
    double recurrence_pair_recall{};
    double recurrence_pair_f1{};
    friend bool operator==(const DiagnosticEvaluationReport&,
                           const DiagnosticEvaluationReport&) = default;
};

struct DiagnosticQualification {
    bool passed{};
    double minimum_precision{0.80};
    double minimum_supported_recall{0.60};
    double minimum_unknown_abstention{0.90};
    double minimum_top3_contributor{0.70};
};

[[nodiscard]] DiagnosticQualification qualify_v0151(
    const DiagnosticEvaluationReport& report) noexcept;

enum class DiagnosticEvaluationErrorCode : std::uint8_t {
    corpus_not_frozen,
    invalid_prediction,
    duplicate_prediction,
    prediction_without_truth,
    no_truth_for_split,
};

struct DiagnosticEvaluationError {
    DiagnosticEvaluationErrorCode code{
        DiagnosticEvaluationErrorCode::invalid_prediction};
    std::string message{};
    friend bool operator==(const DiagnosticEvaluationError&,
                           const DiagnosticEvaluationError&) = default;
};

[[nodiscard]] std::expected<DiagnosticEvaluationReport, DiagnosticEvaluationError>
evaluate_diagnostics(const DogfoodCorpus& corpus,
                     const std::vector<DiagnosticPrediction>& predictions,
                     CorpusSplit split = CorpusSplit::held_out) noexcept;

struct CalibrationSample {
    double confidence{};
    bool correct{};
    friend bool operator==(const CalibrationSample&, const CalibrationSample&) = default;
};

struct ConfidenceCalibrationKnot {
    double maximum_input{};
    double calibrated_probability{};
    std::size_t sample_count{};
    friend bool operator==(const ConfidenceCalibrationKnot&,
                           const ConfidenceCalibrationKnot&) = default;
};

struct ConfidenceCalibrationModel {
    std::size_t source_samples{};
    std::vector<ConfidenceCalibrationKnot> knots{};
    friend bool operator==(const ConfidenceCalibrationModel&,
                           const ConfidenceCalibrationModel&) = default;
};

struct AssertionThresholdSelection {
    bool assertions_enabled{};
    double minimum_precision{0.80};
    double threshold{1.0};
    std::size_t calibration_rows{};
    std::size_t asserted_rows{};
    double observed_precision{};
    friend bool operator==(const AssertionThresholdSelection&,
                           const AssertionThresholdSelection&) = default;
};

[[nodiscard]] std::optional<ConfidenceCalibrationModel>
fit_isotonic_confidence_calibration(
    std::vector<CalibrationSample> samples) noexcept;

[[nodiscard]] double apply_confidence_calibration(
    const ConfidenceCalibrationModel& model, double confidence) noexcept;

// Selects the widest-coverage assertion threshold that reaches the
// predeclared calibration precision. If no threshold qualifies, assertions
// remain disabled instead of weakening the precision requirement.
[[nodiscard]] AssertionThresholdSelection select_assertion_threshold(
    std::vector<CalibrationSample> calibrated_samples,
    double minimum_precision = 0.80) noexcept;

} // namespace blackbox::evaluation
