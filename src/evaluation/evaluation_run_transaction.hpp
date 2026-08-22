#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace blackbox::evaluation {

inline constexpr std::uint32_t evaluation_transaction_format_version = 1U;
inline constexpr std::uintmax_t maximum_evaluation_artifact_bytes = 64U * 1024U * 1024U;

enum class EvaluationArtifactErrorCode : std::uint8_t {
    io,
    already_exists,
    invalid_state,
    incomplete,
    limit_exceeded,
};

struct EvaluationArtifactError {
    EvaluationArtifactErrorCode code{EvaluationArtifactErrorCode::io};
    std::string message{};
    friend bool operator==(const EvaluationArtifactError&,
                           const EvaluationArtifactError&) = default;
};

struct EvaluationOutputTransaction {
    std::filesystem::path final_directory{};
    std::filesystem::path staging_directory{};
    friend bool operator==(const EvaluationOutputTransaction&,
                           const EvaluationOutputTransaction&) = default;
};

enum class HeldOutEvaluationState : std::uint8_t {
    not_started,
    running,
    complete,
};

struct HeldOutEvaluationAttempt {
    std::filesystem::path lock_directory{};
    std::uint64_t annotation_fingerprint{};
    std::uint64_t configuration_fingerprint{};
    std::uint64_t calibration_artifact_fingerprint{};
    friend bool operator==(const HeldOutEvaluationAttempt&,
                           const HeldOutEvaluationAttempt&) = default;
};

struct HeldOutEvaluationStatus {
    HeldOutEvaluationState state{HeldOutEvaluationState::not_started};
    std::uint64_t annotation_fingerprint{};
    std::uint64_t configuration_fingerprint{};
    std::uint64_t calibration_artifact_fingerprint{};
    std::optional<bool> qualification_passed{};
    std::optional<std::uint64_t> report_artifact_fingerprint{};
    friend bool operator==(const HeldOutEvaluationStatus&,
                           const HeldOutEvaluationStatus&) = default;
};

[[nodiscard]] std::expected<void, EvaluationArtifactError>
validate_evaluation_output_destination(
    const std::filesystem::path& final_directory) noexcept;

[[nodiscard]] std::expected<EvaluationOutputTransaction, EvaluationArtifactError>
begin_evaluation_output(
    const std::filesystem::path& final_directory) noexcept;

[[nodiscard]] std::expected<void, EvaluationArtifactError>
publish_evaluation_output(
    const EvaluationOutputTransaction& transaction,
    std::span<const std::string_view> required_files) noexcept;

[[nodiscard]] std::expected<std::uint64_t, EvaluationArtifactError>
evaluation_artifact_fingerprint(
    std::span<const std::filesystem::path> files) noexcept;

[[nodiscard]] std::expected<HeldOutEvaluationAttempt, EvaluationArtifactError>
acquire_held_out_evaluation_attempt(
    const std::filesystem::path& corpus_directory,
    std::uint64_t annotation_fingerprint,
    std::uint64_t configuration_fingerprint,
    std::uint64_t calibration_artifact_fingerprint) noexcept;

[[nodiscard]] std::expected<void, EvaluationArtifactError>
complete_held_out_evaluation_attempt(
    const HeldOutEvaluationAttempt& attempt,
    bool qualification_passed,
    std::uint64_t report_artifact_fingerprint) noexcept;

[[nodiscard]] std::expected<HeldOutEvaluationStatus, EvaluationArtifactError>
held_out_evaluation_status(
    const std::filesystem::path& corpus_directory) noexcept;

} // namespace blackbox::evaluation
