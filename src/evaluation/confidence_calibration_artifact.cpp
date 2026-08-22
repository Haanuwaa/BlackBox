#include "evaluation/confidence_calibration_artifact.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

namespace blackbox::evaluation {
namespace {

constexpr std::string_view knot_header{
    "maximum_input\tcalibrated_probability\tsample_count"};

[[nodiscard]] ConfidenceCalibrationArtifactError error(
    const ConfidenceCalibrationArtifactErrorCode code, std::string message) {
    return {code, std::move(message)};
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

[[nodiscard]] std::optional<double> number(const std::string_view text) noexcept {
    double value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value,
                                        std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::string_view> field(
    const std::string_view line, const std::string_view name) noexcept {
    if (!line.starts_with(name) || line.size() <= name.size() ||
        line[name.size()] != '=') {
        return std::nullopt;
    }
    return line.substr(name.size() + 1U);
}

[[nodiscard]] bool valid_artifact(
    const ConfidenceCalibrationArtifact& artifact) noexcept {
    if (artifact.annotation_fingerprint == 0U ||
        artifact.configuration_fingerprint == 0U ||
        artifact.model.source_samples < 10U || artifact.model.knots.empty() ||
        artifact.model.knots.size() > maximum_calibration_knots ||
        artifact.assertion.calibration_rows != artifact.model.source_samples ||
        !std::isfinite(artifact.assertion.minimum_precision) ||
        artifact.assertion.minimum_precision <= 0.5 ||
        artifact.assertion.minimum_precision > 1.0 ||
        !std::isfinite(artifact.assertion.threshold) ||
        artifact.assertion.threshold < 0.0 || artifact.assertion.threshold > 1.0 ||
        artifact.assertion.asserted_rows > artifact.model.source_samples ||
        !std::isfinite(artifact.assertion.observed_precision) ||
        artifact.assertion.observed_precision < 0.0 ||
        artifact.assertion.observed_precision > 1.0 ||
        (artifact.assertion.assertions_enabled &&
         (artifact.assertion.asserted_rows == 0U ||
          artifact.assertion.observed_precision <
              artifact.assertion.minimum_precision)) ||
        (!artifact.assertion.assertions_enabled &&
         (artifact.assertion.asserted_rows != 0U ||
          artifact.assertion.observed_precision != 0.0 ||
          artifact.assertion.threshold != 1.0))) {
        return false;
    }
    double previous_maximum{-1.0};
    double previous_probability{-1.0};
    std::size_t represented{};
    for (const auto& knot : artifact.model.knots) {
        if (!std::isfinite(knot.maximum_input) || knot.maximum_input < 0.0 ||
            knot.maximum_input > 1.0 ||
            !std::isfinite(knot.calibrated_probability) ||
            knot.calibrated_probability < 0.0 ||
            knot.calibrated_probability > 1.0 || knot.sample_count == 0U ||
            knot.maximum_input < previous_maximum ||
            knot.calibrated_probability < previous_probability ||
            represented > (std::numeric_limits<std::size_t>::max)() -
                              knot.sample_count) {
            return false;
        }
        represented += knot.sample_count;
        previous_maximum = knot.maximum_input;
        previous_probability = knot.calibrated_probability;
    }
    return represented == artifact.model.source_samples;
}

[[nodiscard]] std::expected<std::string, ConfidenceCalibrationArtifactError>
read_bounded(const std::filesystem::path& path) {
    std::error_code issue;
    const auto status = std::filesystem::symlink_status(path, issue);
    if (issue || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        return std::unexpected{error(
            ConfidenceCalibrationArtifactErrorCode::invalid_format,
            "calibration artifact must be an existing non-link regular file")};
    }
    const auto size = std::filesystem::file_size(path, issue);
    if (issue || size == 0U) {
        return std::unexpected{error(
            ConfidenceCalibrationArtifactErrorCode::invalid_format,
            "calibration artifact is empty or unreadable")};
    }
    if (size > maximum_confidence_calibration_artifact_bytes) {
        return std::unexpected{error(
            ConfidenceCalibrationArtifactErrorCode::limit_exceeded,
            "calibration artifact exceeds its direct-V1 byte bound")};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected{error(ConfidenceCalibrationArtifactErrorCode::io,
                                     "cannot open calibration artifact")};
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        return std::unexpected{error(ConfidenceCalibrationArtifactErrorCode::io,
                                     "cannot read calibration artifact exactly")};
    }
    return bytes;
}

[[nodiscard]] std::expected<std::vector<std::string_view>,
                            ConfidenceCalibrationArtifactError>
lines(const std::string_view text) {
    if (text.empty() || text.back() != '\n') {
        return std::unexpected{error(
            ConfidenceCalibrationArtifactErrorCode::invalid_format,
            "calibration artifact must be LF terminated")};
    }
    std::vector<std::string_view> result;
    std::size_t begin{};
    while (begin < text.size()) {
        const auto end = text.find('\n', begin);
        if (end == std::string_view::npos) break;
        const auto line = text.substr(begin, end - begin);
        if (line.empty() || line.size() > maximum_confidence_calibration_line_bytes ||
            line.find('\r') != std::string_view::npos) {
            return std::unexpected{error(
                ConfidenceCalibrationArtifactErrorCode::invalid_format,
                "calibration artifact contains a blank, oversized, or CR line")};
        }
        result.push_back(line);
        begin = end + 1U;
    }
    return result;
}

} // namespace

std::expected<std::string, ConfidenceCalibrationArtifactError>
serialize_confidence_calibration_artifact(
    const ConfidenceCalibrationArtifact& artifact) noexcept {
    try {
        if (!valid_artifact(artifact)) {
            return std::unexpected{error(
                ConfidenceCalibrationArtifactErrorCode::invalid_format,
                "confidence calibration values violate direct V1")};
        }
        std::ostringstream output;
        output << std::setprecision(17)
               << "format=blackbox-confidence-calibration\n"
               << "version=" << confidence_calibration_artifact_format_version << '\n'
               << "annotation_fingerprint=" << artifact.annotation_fingerprint << '\n'
               << "configuration_fingerprint="
               << artifact.configuration_fingerprint << '\n'
               << "source_split=calibration\n"
               << "source_samples=" << artifact.model.source_samples << '\n'
               << "assertions_enabled="
               << (artifact.assertion.assertions_enabled ? 1 : 0) << '\n'
               << "minimum_assertion_precision="
               << artifact.assertion.minimum_precision << '\n'
               << "assertion_threshold=" << artifact.assertion.threshold << '\n'
               << "asserted_calibration_rows="
               << artifact.assertion.asserted_rows << '\n'
               << "observed_assertion_precision="
               << artifact.assertion.observed_precision << '\n'
               << knot_header << '\n';
        for (const auto& knot : artifact.model.knots) {
            output << knot.maximum_input << '\t' << knot.calibrated_probability
                   << '\t' << knot.sample_count << '\n';
        }
        auto result = output.str();
        if (result.size() > maximum_confidence_calibration_artifact_bytes) {
            return std::unexpected{error(
                ConfidenceCalibrationArtifactErrorCode::limit_exceeded,
                "serialized calibration artifact exceeds its byte bound")};
        }
        return result;
    } catch (const std::exception& exception) {
        return std::unexpected{error(ConfidenceCalibrationArtifactErrorCode::io,
                                     exception.what())};
    } catch (...) {
        return std::unexpected{error(ConfidenceCalibrationArtifactErrorCode::io,
                                     "unknown calibration serialization failure")};
    }
}

std::expected<ConfidenceCalibrationArtifact, ConfidenceCalibrationArtifactError>
load_confidence_calibration_artifact(
    const std::filesystem::path& path) noexcept {
    try {
        auto bytes = read_bounded(path);
        if (!bytes) return std::unexpected{bytes.error()};
        auto rows = lines(*bytes);
        if (!rows) return std::unexpected{rows.error()};
        if (rows->size() < 13U || rows->size() > 12U + maximum_calibration_knots ||
            (*rows)[0U] != "format=blackbox-confidence-calibration" ||
            (*rows)[1U] != "version=" +
                              std::to_string(
                                  confidence_calibration_artifact_format_version) ||
            (*rows)[4U] != "source_split=calibration" ||
            (*rows)[11U] != knot_header) {
            return std::unexpected{error(
                ConfidenceCalibrationArtifactErrorCode::invalid_format,
                "calibration artifact structure does not match direct V1")};
        }
        const auto annotation_text = field((*rows)[2U], "annotation_fingerprint");
        const auto configuration_text = field((*rows)[3U], "configuration_fingerprint");
        const auto source_text = field((*rows)[5U], "source_samples");
        const auto enabled_text = field((*rows)[6U], "assertions_enabled");
        const auto minimum_text = field((*rows)[7U], "minimum_assertion_precision");
        const auto threshold_text = field((*rows)[8U], "assertion_threshold");
        const auto asserted_text = field((*rows)[9U], "asserted_calibration_rows");
        const auto observed_text = field((*rows)[10U], "observed_assertion_precision");
        if (!annotation_text || !configuration_text || !source_text ||
            !enabled_text || !minimum_text || !threshold_text || !asserted_text ||
            !observed_text) {
            return std::unexpected{error(
                ConfidenceCalibrationArtifactErrorCode::invalid_format,
                "calibration artifact fields are missing or reordered")};
        }
        const auto annotation = integer<std::uint64_t>(*annotation_text);
        const auto configuration = integer<std::uint64_t>(*configuration_text);
        const auto source = integer<std::size_t>(*source_text);
        const auto enabled = integer<unsigned>(*enabled_text);
        const auto minimum = number(*minimum_text);
        const auto threshold = number(*threshold_text);
        const auto asserted = integer<std::size_t>(*asserted_text);
        const auto observed = number(*observed_text);
        if (!annotation || !configuration || !source || !enabled || *enabled > 1U ||
            !minimum || !threshold || !asserted || !observed) {
            return std::unexpected{error(
                ConfidenceCalibrationArtifactErrorCode::invalid_format,
                "calibration artifact scalar value is invalid")};
        }
        ConfidenceCalibrationArtifact artifact{
            *annotation, *configuration, ConfidenceCalibrationModel{*source, {}},
            AssertionThresholdSelection{*enabled != 0U, *minimum, *threshold,
                                        *source, *asserted, *observed}};
        for (std::size_t index = 12U; index < rows->size(); ++index) {
            const auto first = (*rows)[index].find('\t');
            const auto second = first == std::string_view::npos
                ? first : (*rows)[index].find('\t', first + 1U);
            if (first == std::string_view::npos || second == std::string_view::npos ||
                (*rows)[index].find('\t', second + 1U) != std::string_view::npos) {
                return std::unexpected{error(
                    ConfidenceCalibrationArtifactErrorCode::invalid_format,
                    "calibration knot row does not have three fields")};
            }
            const auto maximum = number((*rows)[index].substr(0U, first));
            const auto probability = number((*rows)[index].substr(
                first + 1U, second - first - 1U));
            const auto count = integer<std::size_t>(
                (*rows)[index].substr(second + 1U));
            if (!maximum || !probability || !count) {
                return std::unexpected{error(
                    ConfidenceCalibrationArtifactErrorCode::invalid_format,
                    "calibration knot value is invalid")};
            }
            artifact.model.knots.push_back({*maximum, *probability, *count});
        }
        if (!valid_artifact(artifact)) {
            return std::unexpected{error(
                ConfidenceCalibrationArtifactErrorCode::invalid_format,
                "calibration artifact values violate direct V1")};
        }
        auto canonical = serialize_confidence_calibration_artifact(artifact);
        if (!canonical || *canonical != *bytes) {
            return std::unexpected{error(
                ConfidenceCalibrationArtifactErrorCode::noncanonical,
                "calibration artifact is not the canonical direct-V1 representation")};
        }
        return artifact;
    } catch (const std::exception& exception) {
        return std::unexpected{error(ConfidenceCalibrationArtifactErrorCode::io,
                                     exception.what())};
    } catch (...) {
        return std::unexpected{error(ConfidenceCalibrationArtifactErrorCode::io,
                                     "unknown calibration parsing failure")};
    }
}

std::expected<void, ConfidenceCalibrationArtifactError>
write_confidence_calibration_artifact(
    const std::filesystem::path& path,
    const ConfidenceCalibrationArtifact& artifact) noexcept {
    try {
        auto bytes = serialize_confidence_calibration_artifact(artifact);
        if (!bytes) return std::unexpected{bytes.error()};
        const auto partial = path.parent_path() /
                             (path.filename().string() + ".partial");
        std::error_code issue;
        if (!std::filesystem::is_directory(path.parent_path(), issue) || issue ||
            std::filesystem::exists(path, issue) || issue ||
            std::filesystem::exists(partial, issue) || issue) {
            return std::unexpected{error(
                ConfidenceCalibrationArtifactErrorCode::already_exists,
                "calibration destination or staging path is occupied")};
        }
        std::ofstream output(partial, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected{error(ConfidenceCalibrationArtifactErrorCode::io,
                                         "cannot create calibration staging file")};
        }
        output.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
        output.flush();
        if (!output) {
            return std::unexpected{error(ConfidenceCalibrationArtifactErrorCode::io,
                                         "cannot commit calibration staging file")};
        }
        output.close();
        std::filesystem::rename(partial, path, issue);
        if (issue) {
            return std::unexpected{error(ConfidenceCalibrationArtifactErrorCode::io,
                                         "cannot publish calibration artifact")};
        }
        auto reloaded = load_confidence_calibration_artifact(path);
        if (!reloaded || *reloaded != artifact) {
            return std::unexpected{error(ConfidenceCalibrationArtifactErrorCode::io,
                                         "published calibration failed verification")};
        }
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected{error(ConfidenceCalibrationArtifactErrorCode::io,
                                     exception.what())};
    } catch (...) {
        return std::unexpected{error(ConfidenceCalibrationArtifactErrorCode::io,
                                     "unknown calibration write failure")};
    }
}

} // namespace blackbox::evaluation
