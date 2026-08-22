#pragma once

#include "evaluation/diagnostic_evaluation.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace blackbox::evaluation {

inline constexpr std::uintmax_t maximum_confidence_calibration_artifact_bytes =
    64U * 1024U;
inline constexpr std::size_t maximum_confidence_calibration_line_bytes = 4U * 1024U;

struct ConfidenceCalibrationArtifact {
    std::uint64_t annotation_fingerprint{};
    std::uint64_t configuration_fingerprint{};
    ConfidenceCalibrationModel model{};
    AssertionThresholdSelection assertion{};
    friend bool operator==(const ConfidenceCalibrationArtifact&,
                           const ConfidenceCalibrationArtifact&) = default;
};

enum class ConfidenceCalibrationArtifactErrorCode : std::uint8_t {
    io,
    limit_exceeded,
    invalid_format,
    noncanonical,
    already_exists,
};

struct ConfidenceCalibrationArtifactError {
    ConfidenceCalibrationArtifactErrorCode code{
        ConfidenceCalibrationArtifactErrorCode::invalid_format};
    std::string message{};
    friend bool operator==(const ConfidenceCalibrationArtifactError&,
                           const ConfidenceCalibrationArtifactError&) = default;
};

[[nodiscard]] std::expected<std::string, ConfidenceCalibrationArtifactError>
serialize_confidence_calibration_artifact(
    const ConfidenceCalibrationArtifact& artifact) noexcept;

// Accepts only the one LF-terminated canonical direct-V1 representation from
// an existing non-link regular file within the fixed byte/line bounds.
[[nodiscard]] std::expected<ConfidenceCalibrationArtifact,
                            ConfidenceCalibrationArtifactError>
load_confidence_calibration_artifact(
    const std::filesystem::path& path) noexcept;

// Creates a new canonical artifact and refuses to overwrite an occupied path.
[[nodiscard]] std::expected<void, ConfidenceCalibrationArtifactError>
write_confidence_calibration_artifact(
    const std::filesystem::path& path,
    const ConfidenceCalibrationArtifact& artifact) noexcept;

} // namespace blackbox::evaluation
