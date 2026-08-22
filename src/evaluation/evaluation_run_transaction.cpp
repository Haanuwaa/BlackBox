#include "evaluation/evaluation_run_transaction.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <map>
#include <set>
#include <vector>

namespace blackbox::evaluation {
namespace {

constexpr std::string_view attempt_format{"blackbox-heldout-evaluation-attempt"};
constexpr std::string_view result_format{"blackbox-heldout-evaluation-result"};

[[nodiscard]] EvaluationArtifactError error(
    const EvaluationArtifactErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] std::filesystem::path staging_path(
    const std::filesystem::path& final_directory) {
    return final_directory.parent_path() /
           (final_directory.filename().string() + ".partial");
}

[[nodiscard]] bool simple_filename(const std::string_view value) noexcept {
    if (value.empty() || value == "." || value == "..") return false;
    return value.find('/') == std::string_view::npos &&
           value.find('\\') == std::string_view::npos;
}

[[nodiscard]] std::expected<void, EvaluationArtifactError> write_text(
    const std::filesystem::path& path, const std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                     "cannot create evaluation state file")};
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                     "cannot commit evaluation state file")};
    }
    return {};
}

[[nodiscard]] std::expected<void, EvaluationArtifactError> write_text_atomically(
    const std::filesystem::path& final_path, const std::string_view text) {
    const auto temporary = final_path.parent_path() /
                           (final_path.filename().string() + ".partial");
    std::error_code issue;
    if (std::filesystem::exists(final_path, issue) || issue ||
        std::filesystem::exists(temporary, issue) || issue) {
        return std::unexpected{error(EvaluationArtifactErrorCode::already_exists,
                                     "evaluation state file already exists")};
    }
    if (auto written = write_text(temporary, text); !written) return written;
    std::filesystem::rename(temporary, final_path, issue);
    if (issue) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                     "cannot publish evaluation state file")};
    }
    return {};
}

[[nodiscard]] std::expected<std::map<std::string, std::string>, EvaluationArtifactError>
read_fields(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                     "evaluation state file is missing")};
    }
    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto separator = line.find('=');
        if (separator == std::string::npos || separator == 0U ||
            !fields.emplace(line.substr(0U, separator),
                            line.substr(separator + 1U)).second) {
            return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                         "evaluation state file is malformed")};
        }
    }
    if (!input.eof()) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                     "cannot read evaluation state file")};
    }
    return fields;
}

template <typename Value>
[[nodiscard]] std::optional<Value> integer(const std::string_view text) noexcept {
    Value value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::expected<HeldOutEvaluationStatus, EvaluationArtifactError>
parse_attempt(const std::filesystem::path& lock_directory) {
    auto attempt = read_fields(lock_directory / "attempt.ini");
    if (!attempt) return std::unexpected{attempt.error()};
    constexpr std::array<std::string_view, 5U> required{
        "format", "version", "annotation_fingerprint",
        "configuration_fingerprint", "calibration_artifact_fingerprint"};
    if (attempt->size() != required.size() ||
        !std::all_of(required.begin(), required.end(), [&](const auto name) {
            return attempt->contains(std::string{name});
        }) || (*attempt)["format"] != attempt_format ||
        (*attempt)["version"] != std::to_string(evaluation_transaction_format_version)) {
        return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                     "held-out attempt contract is invalid")};
    }
    const auto annotation = integer<std::uint64_t>((*attempt)["annotation_fingerprint"]);
    const auto configuration = integer<std::uint64_t>((*attempt)["configuration_fingerprint"]);
    const auto calibration = integer<std::uint64_t>(
        (*attempt)["calibration_artifact_fingerprint"]);
    if (!annotation || *annotation == 0U || !configuration || *configuration == 0U ||
        !calibration || *calibration == 0U) {
        return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                     "held-out attempt provenance is invalid")};
    }
    return HeldOutEvaluationStatus{HeldOutEvaluationState::running, *annotation,
                                   *configuration, *calibration, std::nullopt,
                                   std::nullopt};
}

void fingerprint_bytes(std::uint64_t& hash, const std::string_view bytes) noexcept {
    for (const auto byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1'099'511'628'211ULL;
    }
    hash ^= 0xFFU;
    hash *= 1'099'511'628'211ULL;
}

} // namespace

std::expected<void, EvaluationArtifactError>
validate_evaluation_output_destination(
    const std::filesystem::path& final_directory) noexcept {
    try {
        if (final_directory.empty() || final_directory.filename().empty() ||
            final_directory.filename() == "." || final_directory.filename() == "..") {
            return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                         "evaluation output path is invalid")};
        }
        const auto final = std::filesystem::absolute(final_directory).lexically_normal();
        const auto staging = staging_path(final);
        std::error_code issue;
        if (!std::filesystem::is_directory(final.parent_path(), issue) || issue) {
            return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                         "evaluation output parent must already exist")};
        }
        if (std::filesystem::exists(final, issue) || issue ||
            std::filesystem::exists(staging, issue) || issue) {
            return std::unexpected{error(EvaluationArtifactErrorCode::already_exists,
                                         "evaluation output or partial directory already exists")};
        }
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                     "unknown output validation failure")};
    }
}

std::expected<EvaluationOutputTransaction, EvaluationArtifactError>
begin_evaluation_output(const std::filesystem::path& final_directory) noexcept {
    if (auto valid = validate_evaluation_output_destination(final_directory); !valid) {
        return std::unexpected{valid.error()};
    }
    try {
        const auto final = std::filesystem::absolute(final_directory).lexically_normal();
        const auto staging = staging_path(final);
        std::error_code issue;
        if (!std::filesystem::create_directory(staging, issue) || issue) {
            return std::unexpected{error(EvaluationArtifactErrorCode::already_exists,
                                         "cannot exclusively create evaluation staging directory")};
        }
        return EvaluationOutputTransaction{final, staging};
    } catch (const std::exception& exception) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                     "unknown output transaction failure")};
    }
}

std::expected<void, EvaluationArtifactError> publish_evaluation_output(
    const EvaluationOutputTransaction& transaction,
    const std::span<const std::string_view> required_files) noexcept {
    try {
        const auto final = std::filesystem::absolute(
            transaction.final_directory).lexically_normal();
        const auto staging = std::filesystem::absolute(
            transaction.staging_directory).lexically_normal();
        if (required_files.empty() || staging != staging_path(final) ||
            !std::filesystem::is_directory(staging)) {
            return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                         "evaluation output transaction is invalid")};
        }
        std::set<std::string> required;
        for (const auto name : required_files) {
            if (!simple_filename(name) || !required.emplace(name).second) {
                return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                             "required evaluation filename is invalid")};
            }
            std::error_code issue;
            const auto path = staging / name;
            if (!std::filesystem::is_regular_file(path, issue) || issue ||
                std::filesystem::file_size(path, issue) == 0U || issue ||
                std::filesystem::file_size(path, issue) >
                    maximum_evaluation_artifact_bytes || issue) {
                return std::unexpected{error(EvaluationArtifactErrorCode::incomplete,
                                             "required evaluation artifact is absent or invalid")};
            }
        }
        for (const auto& entry : std::filesystem::directory_iterator(staging)) {
            if (!entry.is_regular_file() ||
                !required.contains(entry.path().filename().string())) {
                return std::unexpected{error(EvaluationArtifactErrorCode::incomplete,
                                             "evaluation staging directory has unexpected content")};
            }
        }
        std::error_code issue;
        if (std::filesystem::exists(final, issue) || issue) {
            return std::unexpected{error(EvaluationArtifactErrorCode::already_exists,
                                         "evaluation output became occupied before publication")};
        }
        std::filesystem::rename(staging, final, issue);
        if (issue) {
            return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                         "cannot atomically publish evaluation output")};
        }
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                     "unknown output publication failure")};
    }
}

std::expected<std::uint64_t, EvaluationArtifactError>
evaluation_artifact_fingerprint(
    const std::span<const std::filesystem::path> files) noexcept {
    try {
        if (files.empty()) {
            return std::unexpected{error(EvaluationArtifactErrorCode::incomplete,
                                         "artifact fingerprint needs at least one file")};
        }
        std::vector<std::filesystem::path> ordered{files.begin(), files.end()};
        std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
            return left.filename().generic_string() < right.filename().generic_string();
        });
        std::set<std::string> names;
        std::uintmax_t total_bytes{};
        std::uint64_t hash{1'469'598'103'934'665'603ULL};
        // Artifact sets are bounded, but the streaming scratch space belongs on
        // the heap so evaluation tooling does not consume an entire worker stack.
        std::vector<char> buffer(64U * 1024U);
        for (const auto& path : ordered) {
            std::error_code issue;
            if (!std::filesystem::is_regular_file(path, issue) || issue) {
                return std::unexpected{error(EvaluationArtifactErrorCode::incomplete,
                                             "artifact file is missing")};
            }
            const auto size = std::filesystem::file_size(path, issue);
            if (issue || size == 0U) {
                return std::unexpected{error(EvaluationArtifactErrorCode::incomplete,
                                             "artifact file is empty or unreadable")};
            }
            if (size > maximum_evaluation_artifact_bytes ||
                total_bytes > maximum_evaluation_artifact_bytes - size) {
                return std::unexpected{error(EvaluationArtifactErrorCode::limit_exceeded,
                                             "artifact fingerprint exceeds its byte bound")};
            }
            const auto name = path.filename().generic_string();
            if (!names.insert(name).second) {
                return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                             "artifact filenames must be unique")};
            }
            total_bytes += size;
            fingerprint_bytes(hash, name);
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                             "cannot open artifact for fingerprinting")};
            }
            while (input) {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = input.gcount();
                if (count > 0) {
                    fingerprint_bytes(hash, std::string_view{
                        buffer.data(), static_cast<std::size_t>(count)});
                }
            }
            if (!input.eof()) {
                return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                             "cannot read artifact for fingerprinting")};
            }
        }
        return hash;
    } catch (const std::exception& exception) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                     "unknown artifact fingerprint failure")};
    }
}

std::expected<HeldOutEvaluationAttempt, EvaluationArtifactError>
acquire_held_out_evaluation_attempt(
    const std::filesystem::path& corpus_directory,
    const std::uint64_t annotation_fingerprint,
    const std::uint64_t configuration_fingerprint,
    const std::uint64_t calibration_artifact_fingerprint) noexcept {
    try {
        if (annotation_fingerprint == 0U || configuration_fingerprint == 0U ||
            calibration_artifact_fingerprint == 0U ||
            !std::filesystem::is_directory(corpus_directory)) {
            return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                         "held-out attempt provenance is invalid")};
        }
        const auto lock = std::filesystem::absolute(
            corpus_directory / "heldout-evaluation.lock").lexically_normal();
        std::error_code issue;
        if (!std::filesystem::create_directory(lock, issue) || issue) {
            return std::unexpected{error(EvaluationArtifactErrorCode::already_exists,
                                         "held-out evaluation was already started")};
        }
        const auto text = std::string{"format="} + std::string{attempt_format} +
            "\nversion=" + std::to_string(evaluation_transaction_format_version) +
            "\nannotation_fingerprint=" + std::to_string(annotation_fingerprint) +
            "\nconfiguration_fingerprint=" + std::to_string(configuration_fingerprint) +
            "\ncalibration_artifact_fingerprint=" +
            std::to_string(calibration_artifact_fingerprint) + "\n";
        if (auto written = write_text_atomically(lock / "attempt.ini", text); !written) {
            return std::unexpected{written.error()};
        }
        return HeldOutEvaluationAttempt{lock, annotation_fingerprint,
                                        configuration_fingerprint,
                                        calibration_artifact_fingerprint};
    } catch (const std::exception& exception) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                     "unknown held-out attempt failure")};
    }
}

std::expected<void, EvaluationArtifactError>
complete_held_out_evaluation_attempt(
    const HeldOutEvaluationAttempt& attempt,
    const bool qualification_passed,
    const std::uint64_t report_artifact_fingerprint) noexcept {
    try {
        if (report_artifact_fingerprint == 0U) {
            return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                         "held-out report fingerprint is invalid")};
        }
        auto status = parse_attempt(attempt.lock_directory);
        if (!status || status->annotation_fingerprint != attempt.annotation_fingerprint ||
            status->configuration_fingerprint != attempt.configuration_fingerprint ||
            status->calibration_artifact_fingerprint !=
                attempt.calibration_artifact_fingerprint ||
            std::filesystem::exists(attempt.lock_directory / "result.ini")) {
            return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                         "held-out attempt cannot be completed")};
        }
        const auto text = std::string{"format="} + std::string{result_format} +
            "\nversion=" + std::to_string(evaluation_transaction_format_version) +
            "\nqualification_passed=" + (qualification_passed ? "1" : "0") +
            "\nreport_artifact_fingerprint=" +
            std::to_string(report_artifact_fingerprint) + "\n";
        return write_text_atomically(attempt.lock_directory / "result.ini", text);
    } catch (const std::exception& exception) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                     "unknown held-out completion failure")};
    }
}

std::expected<HeldOutEvaluationStatus, EvaluationArtifactError>
held_out_evaluation_status(
    const std::filesystem::path& corpus_directory) noexcept {
    try {
        const auto lock = std::filesystem::absolute(
            corpus_directory / "heldout-evaluation.lock").lexically_normal();
        std::error_code issue;
        if (!std::filesystem::exists(lock, issue) && !issue) {
            return HeldOutEvaluationStatus{};
        }
        if (issue || !std::filesystem::is_directory(lock, issue) || issue) {
            return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                         "held-out lock is not a valid directory")};
        }
        auto status = parse_attempt(lock);
        if (!status) return std::unexpected{status.error()};
        if (!std::filesystem::exists(lock / "result.ini", issue) && !issue) {
            return status;
        }
        if (issue) {
            return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                         "cannot inspect held-out result state")};
        }
        auto result = read_fields(lock / "result.ini");
        if (!result) return std::unexpected{result.error()};
        constexpr std::array<std::string_view, 4U> required{
            "format", "version", "qualification_passed",
            "report_artifact_fingerprint"};
        if (result->size() != required.size() ||
            !std::all_of(required.begin(), required.end(), [&](const auto name) {
                return result->contains(std::string{name});
            }) || (*result)["format"] != result_format ||
            (*result)["version"] !=
                std::to_string(evaluation_transaction_format_version) ||
            ((*result)["qualification_passed"] != "0" &&
             (*result)["qualification_passed"] != "1")) {
            return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                         "held-out result contract is invalid")};
        }
        const auto fingerprint = integer<std::uint64_t>(
            (*result)["report_artifact_fingerprint"]);
        if (!fingerprint || *fingerprint == 0U) {
            return std::unexpected{error(EvaluationArtifactErrorCode::invalid_state,
                                         "held-out result fingerprint is invalid")};
        }
        status->state = HeldOutEvaluationState::complete;
        status->qualification_passed = (*result)["qualification_passed"] == "1";
        status->report_artifact_fingerprint = *fingerprint;
        return status;
    } catch (const std::exception& exception) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(EvaluationArtifactErrorCode::io,
                                     "unknown held-out status failure")};
    }
}

} // namespace blackbox::evaluation
