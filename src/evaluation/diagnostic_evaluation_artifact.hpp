#pragma once

#include "evaluation/diagnostic_evaluation.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace blackbox::evaluation {

inline constexpr std::uintmax_t maximum_diagnostic_evaluation_json_bytes =
    4U * 1024U * 1024U;
inline constexpr std::uintmax_t maximum_diagnostic_predictions_bytes =
    16U * 1024U * 1024U;
inline constexpr std::size_t maximum_diagnostic_prediction_row_bytes = 4U * 1024U;

struct DiagnosticEvaluationArtifactMetadata {
    bool calibration_applied{};
    std::uint64_t calibration_artifact_fingerprint{};
    bool assertions_enabled{};
    double assertion_threshold{};
    friend bool operator==(const DiagnosticEvaluationArtifactMetadata&,
                           const DiagnosticEvaluationArtifactMetadata&) = default;
};

enum class DiagnosticEvaluationArtifactErrorCode : std::uint8_t {
    io,
    limit_exceeded,
    invalid_format,
    evaluation_failed,
    content_mismatch,
};

struct DiagnosticEvaluationArtifactError {
    DiagnosticEvaluationArtifactErrorCode code{
        DiagnosticEvaluationArtifactErrorCode::invalid_format};
    std::string message{};
    friend bool operator==(const DiagnosticEvaluationArtifactError&,
                           const DiagnosticEvaluationArtifactError&) = default;
};

struct VerifiedDiagnosticEvaluationArtifact {
    CorpusSplit split{CorpusSplit::held_out};
    DiagnosticEvaluationArtifactMetadata metadata{};
    DiagnosticEvaluationReport report{};
    std::size_t prediction_rows{};
    bool qualification_passed{};
    friend bool operator==(const VerifiedDiagnosticEvaluationArtifact&,
                           const VerifiedDiagnosticEvaluationArtifact&) = default;
};

[[nodiscard]] std::expected<std::string, DiagnosticEvaluationArtifactError>
serialize_diagnostic_evaluation_json(
    const DogfoodCorpus& corpus,
    const DiagnosticEvaluationReport& report,
    const DiagnosticEvaluationArtifactMetadata& metadata) noexcept;

[[nodiscard]] std::expected<std::string, DiagnosticEvaluationArtifactError>
serialize_diagnostic_predictions_tsv(
    const std::vector<DiagnosticPrediction>& predictions) noexcept;

// Recomputes the complete report from the frozen corpus and the published
// prediction rows, then requires both artifacts to be the one canonical V1
// byte representation. No analyzer, archive, or mutable evaluation state is
// consulted.
[[nodiscard]] std::expected<VerifiedDiagnosticEvaluationArtifact,
                            DiagnosticEvaluationArtifactError>
verify_diagnostic_evaluation_artifact(
    const std::filesystem::path& directory,
    const DogfoodCorpus& corpus) noexcept;

} // namespace blackbox::evaluation
