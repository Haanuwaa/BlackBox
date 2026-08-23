#include "evaluation/diagnostic_evaluation_artifact.hpp"
#include "evaluation/strict_number_parser.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

namespace blackbox::evaluation {
namespace {

constexpr std::string_view prediction_header{
    "incident_key\tdiagnosis\tconfidence\tcontext\tautomatic_capture\t"
    "recurrence_cluster\tobserved_pressure\tpractical_pressure_score\t"
    "raw_statistical_score\tcontributor_ordinals"};

[[nodiscard]] DiagnosticEvaluationArtifactError error(
    const DiagnosticEvaluationArtifactErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] bool safe_atom(const std::string_view value,
                             const bool allow_empty = false) noexcept {
    if (value.empty()) return allow_empty;
    return value.size() <= 128U &&
           std::all_of(value.begin(), value.end(), [](const char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '-' || character == '_' || character == '.';
           });
}

[[nodiscard]] bool incident_key(const std::string_view value) noexcept {
    return value.size() == 32U &&
           std::all_of(value.begin(), value.end(), [](const char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
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

[[nodiscard]] std::optional<double> number(const std::string_view text) {
    return parse_finite_decimal(text);
}

[[nodiscard]] const char* resource_name(
    const analysis::ResourceKind value) noexcept {
    switch (value) {
    case analysis::ResourceKind::cpu: return "cpu";
    case analysis::ResourceKind::memory: return "memory";
    case analysis::ResourceKind::disk: return "disk";
    case analysis::ResourceKind::network: return "network";
    }
    return nullptr;
}

[[nodiscard]] std::optional<analysis::ResourceKind> resource(
    const std::string_view value) noexcept {
    if (value == "cpu") return analysis::ResourceKind::cpu;
    if (value == "memory") return analysis::ResourceKind::memory;
    if (value == "disk") return analysis::ResourceKind::disk;
    if (value == "network") return analysis::ResourceKind::network;
    return std::nullopt;
}

[[nodiscard]] std::optional<analysis::IncidentType> diagnosis(
    const std::string_view value) noexcept {
    constexpr std::array values{
        analysis::IncidentType::unknown,
        analysis::IncidentType::cpu_pressure,
        analysis::IncidentType::memory_pressure,
        analysis::IncidentType::storage_pressure,
        analysis::IncidentType::network_pressure,
        analysis::IncidentType::multi_resource_pressure,
        analysis::IncidentType::application_crash,
        analysis::IncidentType::application_hang,
        analysis::IncidentType::dns_resolution_timeout,
        analysis::IncidentType::display_driver_recovery,
        analysis::IncidentType::storage_io_retry};
    for (const auto candidate : values) {
        if (value == to_string(candidate)) return candidate;
    }
    return std::nullopt;
}

[[nodiscard]] bool valid_diagnosis(const analysis::IncidentType value) noexcept {
    switch (value) {
    case analysis::IncidentType::unknown:
    case analysis::IncidentType::cpu_pressure:
    case analysis::IncidentType::memory_pressure:
    case analysis::IncidentType::storage_pressure:
    case analysis::IncidentType::network_pressure:
    case analysis::IncidentType::multi_resource_pressure:
    case analysis::IncidentType::application_crash:
    case analysis::IncidentType::application_hang:
    case analysis::IncidentType::dns_resolution_timeout:
    case analysis::IncidentType::display_driver_recovery:
    case analysis::IncidentType::storage_io_retry: return true;
    }
    return false;
}

[[nodiscard]] std::optional<analysis::WorkloadContextKind> context(
    const std::string_view value) noexcept {
    constexpr std::array values{
        analysis::WorkloadContextKind::unknown,
        analysis::WorkloadContextKind::idle,
        analysis::WorkloadContextKind::gaming,
        analysis::WorkloadContextKind::development,
        analysis::WorkloadContextKind::compilation,
        analysis::WorkloadContextKind::video_playback_or_call,
        analysis::WorkloadContextKind::heavy_download,
        analysis::WorkloadContextKind::desktop};
    for (const auto candidate : values) {
        if (value == to_string(candidate)) return candidate;
    }
    return std::nullopt;
}

[[nodiscard]] bool valid_context(
    const analysis::WorkloadContextKind value) noexcept {
    switch (value) {
    case analysis::WorkloadContextKind::unknown:
    case analysis::WorkloadContextKind::idle:
    case analysis::WorkloadContextKind::gaming:
    case analysis::WorkloadContextKind::development:
    case analysis::WorkloadContextKind::compilation:
    case analysis::WorkloadContextKind::video_playback_or_call:
    case analysis::WorkloadContextKind::heavy_download:
    case analysis::WorkloadContextKind::desktop: return true;
    }
    return false;
}

[[nodiscard]] std::vector<std::string_view> split_exact(
    const std::string_view text, const char separator) {
    std::vector<std::string_view> fields;
    std::size_t begin{};
    while (begin <= text.size()) {
        const auto end = text.find(separator, begin);
        if (end == std::string_view::npos) {
            fields.push_back(text.substr(begin));
            break;
        }
        fields.push_back(text.substr(begin, end - begin));
        begin = end + 1U;
    }
    return fields;
}

[[nodiscard]] bool valid_prediction(const DiagnosticPrediction& prediction) noexcept {
    if (!incident_key(prediction.incident_key) ||
        !valid_diagnosis(prediction.diagnosis) ||
        !std::isfinite(prediction.confidence) || prediction.confidence < 0.0 ||
        prediction.confidence > 1.0 || !valid_context(prediction.context) ||
        prediction.contributor_ordinals.size() > 20U ||
        prediction.recurrence_cluster.size() > 64U ||
        !safe_atom(prediction.recurrence_cluster, true) ||
        !std::isfinite(prediction.practical_pressure_score) ||
        prediction.practical_pressure_score < 0.0 ||
        prediction.practical_pressure_score > 1.0 ||
        !std::isfinite(prediction.raw_statistical_score) ||
        prediction.raw_statistical_score < 0.0 ||
        prediction.raw_statistical_score > 1.0 ||
        std::any_of(prediction.contributor_ordinals.begin(),
                    prediction.contributor_ordinals.end(),
                    [](const auto ordinal) { return ordinal >= 8'192U; })) {
        return false;
    }
    if (prediction.observed_pressure &&
        resource_name(*prediction.observed_pressure) == nullptr) {
        return false;
    }
    const std::set<std::size_t> unique_ordinals{
        prediction.contributor_ordinals.begin(),
        prediction.contributor_ordinals.end()};
    return unique_ordinals.size() == prediction.contributor_ordinals.size();
}

void write_rate(std::ostream& output, const char* name,
                const RateMetric& metric, const bool comma = true) {
    output << "    \"" << name << "\":{\"eligible\":" << metric.eligible
           << ",\"successful\":" << metric.successful << ",\"rate\":"
           << metric.rate << '}' << (comma ? "," : "") << '\n';
}

[[nodiscard]] std::expected<std::string, DiagnosticEvaluationArtifactError>
read_bounded(const std::filesystem::path& path, const std::uintmax_t limit) {
    std::error_code issue;
    const auto status = std::filesystem::symlink_status(path, issue);
    if (issue || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        return std::unexpected{error(
            DiagnosticEvaluationArtifactErrorCode::invalid_format,
            "evaluation artifact must be an existing non-link regular file")};
    }
    const auto size = std::filesystem::file_size(path, issue);
    if (issue || size == 0U) {
        return std::unexpected{error(
            DiagnosticEvaluationArtifactErrorCode::invalid_format,
            "evaluation artifact is empty or unreadable")};
    }
    if (size > limit) {
        return std::unexpected{error(
            DiagnosticEvaluationArtifactErrorCode::limit_exceeded,
            "evaluation artifact exceeds its direct-V1 byte bound")};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected{error(DiagnosticEvaluationArtifactErrorCode::io,
                                     "cannot open evaluation artifact")};
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        return std::unexpected{error(DiagnosticEvaluationArtifactErrorCode::io,
                                     "cannot read evaluation artifact exactly")};
    }
    return bytes;
}

[[nodiscard]] std::optional<std::string_view> next_line(
    const std::string_view text, std::size_t& offset) noexcept {
    if (offset >= text.size()) return std::nullopt;
    const auto end = text.find('\n', offset);
    if (end == std::string_view::npos) return std::nullopt;
    const auto line = text.substr(offset, end - offset);
    offset = end + 1U;
    return line;
}

[[nodiscard]] std::optional<std::string_view> scalar(
    const std::string_view line, const std::string_view prefix,
    const std::string_view suffix) noexcept {
    if (!line.starts_with(prefix) || !line.ends_with(suffix) ||
        line.size() < prefix.size() + suffix.size()) {
        return std::nullopt;
    }
    return line.substr(prefix.size(),
                       line.size() - prefix.size() - suffix.size());
}

struct ParsedHeader {
    CorpusSplit split{CorpusSplit::held_out};
    DiagnosticEvaluationArtifactMetadata metadata{};
};

[[nodiscard]] std::optional<ParsedHeader> parse_header(
    const std::string_view json, const DogfoodCorpus& corpus) {
    std::size_t offset{};
    const auto opening = next_line(json, offset);
    const auto format = next_line(json, offset);
    const auto version = next_line(json, offset);
    const auto corpus_line = next_line(json, offset);
    const auto split_line = next_line(json, offset);
    const auto annotation = next_line(json, offset);
    const auto configuration = next_line(json, offset);
    const auto calibration = next_line(json, offset);
    const auto calibration_fingerprint = next_line(json, offset);
    const auto assertions = next_line(json, offset);
    const auto threshold = next_line(json, offset);
    if (!opening || !format || !version || !corpus_line || !split_line ||
        !annotation || !configuration || !calibration || !assertions ||
        !calibration_fingerprint || !threshold || *opening != "{" ||
        *format != "  \"format\":\"blackbox-diagnostic-evaluation\"," ||
        *version != "  \"version\":" +
                        std::to_string(diagnostic_evaluation_report_format_version) +
                        "," ||
        *corpus_line != "  \"corpus_id\":\"" + corpus.manifest.corpus_id +
                            "\"," ||
        *annotation != "  \"annotation_fingerprint\":" +
                           std::to_string(corpus.manifest.annotation_fingerprint) +
                           "," ||
        *configuration != "  \"configuration_fingerprint\":" +
                              std::to_string(
                                  corpus.manifest.configuration_fingerprint) +
                              ",") {
        return std::nullopt;
    }
    const auto split_text = scalar(*split_line, "  \"split\":\"", "\",");
    const auto calibration_text = scalar(
        *calibration, "  \"calibration_applied\":", ",");
    const auto calibration_fingerprint_text = scalar(
        *calibration_fingerprint,
        "  \"calibration_artifact_fingerprint\":", ",");
    const auto assertion_text = scalar(
        *assertions, "  \"assertions_enabled\":", ",");
    const auto threshold_text = scalar(
        *threshold, "  \"assertion_threshold\":", ",");
    if (!split_text || !calibration_text || !calibration_fingerprint_text ||
        !assertion_text || !threshold_text) {
        return std::nullopt;
    }
    ParsedHeader parsed{};
    if (*split_text == "development") parsed.split = CorpusSplit::development;
    else if (*split_text == "calibration") parsed.split = CorpusSplit::calibration;
    else if (*split_text == "held_out") parsed.split = CorpusSplit::held_out;
    else return std::nullopt;
    if (*calibration_text == "true") parsed.metadata.calibration_applied = true;
    else if (*calibration_text != "false") return std::nullopt;
    const auto parsed_calibration_fingerprint = integer<std::uint64_t>(
        *calibration_fingerprint_text);
    if (!parsed_calibration_fingerprint) return std::nullopt;
    parsed.metadata.calibration_artifact_fingerprint =
        *parsed_calibration_fingerprint;
    if (*assertion_text == "true") parsed.metadata.assertions_enabled = true;
    else if (*assertion_text != "false") return std::nullopt;
    const auto parsed_threshold = number(*threshold_text);
    if (!parsed_threshold || *parsed_threshold < 0.0 || *parsed_threshold > 1.0 ||
        (!parsed.metadata.calibration_applied &&
         (parsed.metadata.calibration_artifact_fingerprint != 0U ||
          parsed.metadata.assertions_enabled || *parsed_threshold != 0.0)) ||
        (parsed.metadata.calibration_applied &&
         parsed.metadata.calibration_artifact_fingerprint == 0U) ||
        (parsed.metadata.assertions_enabled &&
         !parsed.metadata.calibration_applied)) {
        return std::nullopt;
    }
    parsed.metadata.assertion_threshold = *parsed_threshold;
    return parsed;
}

[[nodiscard]] std::expected<std::vector<DiagnosticPrediction>,
                            DiagnosticEvaluationArtifactError>
parse_predictions(const std::string_view text, const std::size_t maximum_rows) {
    std::size_t offset{};
    const auto header = next_line(text, offset);
    if (!header || *header != prediction_header) {
        return std::unexpected{error(
            DiagnosticEvaluationArtifactErrorCode::invalid_format,
            "prediction table header does not match direct V1")};
    }
    std::vector<DiagnosticPrediction> predictions;
    while (offset < text.size()) {
        const auto line = next_line(text, offset);
        if (!line || line->empty() ||
            line->size() > maximum_diagnostic_prediction_row_bytes) {
            return std::unexpected{error(
                DiagnosticEvaluationArtifactErrorCode::invalid_format,
                "prediction table row is missing, blank, unterminated, or oversized")};
        }
        if (predictions.size() == maximum_rows) {
            return std::unexpected{error(
                DiagnosticEvaluationArtifactErrorCode::limit_exceeded,
                "prediction table has more rows than selected-split truth")};
        }
        const auto fields = split_exact(*line, '\t');
        if (fields.size() != 10U) {
            return std::unexpected{error(
                DiagnosticEvaluationArtifactErrorCode::invalid_format,
                "prediction table row does not have ten fields")};
        }
        const auto parsed_diagnosis = diagnosis(fields[1U]);
        const auto confidence = number(fields[2U]);
        const auto parsed_context = context(fields[3U]);
        const auto practical = number(fields[7U]);
        const auto raw = number(fields[8U]);
        if (!incident_key(fields[0U]) || !parsed_diagnosis || !confidence ||
            *confidence < 0.0 || *confidence > 1.0 || !parsed_context ||
            (fields[4U] != "0" && fields[4U] != "1") ||
            fields[5U].size() > 64U || !safe_atom(fields[5U], true) ||
            !practical || *practical < 0.0 || *practical > 1.0 ||
            !raw || *raw < 0.0 || *raw > 1.0) {
            return std::unexpected{error(
                DiagnosticEvaluationArtifactErrorCode::invalid_format,
                "prediction table value is outside direct-V1 bounds")};
        }
        std::optional<analysis::ResourceKind> pressure;
        if (fields[6U] != "none") {
            pressure = resource(fields[6U]);
            if (!pressure) {
                return std::unexpected{error(
                    DiagnosticEvaluationArtifactErrorCode::invalid_format,
                    "prediction pressure kind is invalid")};
            }
        }
        std::vector<std::size_t> ordinals;
        if (!fields[9U].empty()) {
            const auto values = split_exact(fields[9U], ',');
            if (values.size() > 20U) {
                return std::unexpected{error(
                    DiagnosticEvaluationArtifactErrorCode::limit_exceeded,
                    "prediction contributor list exceeds its bound")};
            }
            std::set<std::size_t> unique;
            for (const auto value : values) {
                const auto ordinal = integer<std::size_t>(value);
                if (!ordinal || *ordinal >= 8'192U ||
                    !unique.insert(*ordinal).second) {
                    return std::unexpected{error(
                        DiagnosticEvaluationArtifactErrorCode::invalid_format,
                        "prediction contributor ordinals are invalid or duplicate")};
                }
                ordinals.push_back(*ordinal);
            }
        }
        predictions.push_back({std::string{fields[0U]}, *parsed_diagnosis,
                               *confidence, std::move(ordinals), *parsed_context,
                               fields[4U] == "1", std::string{fields[5U]}, pressure,
                               *practical, *raw});
    }
    return predictions;
}

} // namespace

std::expected<std::string, DiagnosticEvaluationArtifactError>
serialize_diagnostic_evaluation_json(
    const DogfoodCorpus& corpus,
    const DiagnosticEvaluationReport& report,
    const DiagnosticEvaluationArtifactMetadata& metadata) noexcept {
    try {
        if (!safe_atom(corpus.manifest.corpus_id) ||
            !std::isfinite(metadata.assertion_threshold) ||
            metadata.assertion_threshold < 0.0 ||
            metadata.assertion_threshold > 1.0 ||
            (!metadata.calibration_applied &&
             (metadata.calibration_artifact_fingerprint != 0U ||
              metadata.assertions_enabled || metadata.assertion_threshold != 0.0)) ||
            (metadata.calibration_applied &&
             metadata.calibration_artifact_fingerprint == 0U) ||
            (metadata.assertions_enabled && !metadata.calibration_applied)) {
            return std::unexpected{error(
                DiagnosticEvaluationArtifactErrorCode::invalid_format,
                "evaluation report metadata is invalid")};
        }
        std::ostringstream output;
        output << std::setprecision(17)
               << "{\n  \"format\":\"blackbox-diagnostic-evaluation\",\n"
               << "  \"version\":"
               << diagnostic_evaluation_report_format_version << ",\n"
               << "  \"corpus_id\":\"" << corpus.manifest.corpus_id << "\",\n"
               << "  \"split\":\"" << to_string(report.split) << "\",\n"
               << "  \"annotation_fingerprint\":"
               << corpus.manifest.annotation_fingerprint << ",\n"
               << "  \"configuration_fingerprint\":"
               << corpus.manifest.configuration_fingerprint << ",\n"
               << "  \"calibration_applied\":"
               << (metadata.calibration_applied ? "true" : "false") << ",\n"
               << "  \"calibration_artifact_fingerprint\":"
               << metadata.calibration_artifact_fingerprint << ",\n"
               << "  \"assertions_enabled\":"
               << (metadata.assertions_enabled ? "true" : "false") << ",\n"
               << "  \"assertion_threshold\":" << metadata.assertion_threshold
               << ",\n"
               << "  \"coverage\":{\"truth_rows\":" << report.truth_rows
               << ",\"predictions_matched\":" << report.predictions_matched
               << ",\"predictions_missing\":" << report.predictions_missing
               << ",\"uncertain_excluded\":" << report.uncertain_excluded
               << ",\"disagreement_excluded\":" << report.disagreement_excluded
               << ",\"hardware_profiles\":"
               << report.hardware_profiles_represented
               << "},\n  \"symptom_counts\":{";
        for (std::size_t index = 0U; index < report.symptom_counts.size(); ++index) {
            if (index != 0U) output << ',';
            output << '\"' << to_string(static_cast<SymptomClass>(index))
                   << "\":" << report.symptom_counts[index];
        }
        output << "},\n  \"hardware_distribution\":[";
        std::set<std::string> represented_profile_ids;
        for (const auto& session : corpus.sessions) {
            if (session.split == report.split) {
                represented_profile_ids.insert(session.hardware_profile_id);
            }
        }
        bool first_profile{true};
        for (const auto& profile : corpus.hardware_profiles) {
            if (!represented_profile_ids.contains(profile.profile_id)) continue;
            if (!safe_atom(profile.profile_id) || !safe_atom(profile.os_family) ||
                !safe_atom(profile.os_build_bucket) || !safe_atom(profile.cpu_family) ||
                !safe_atom(profile.memory_gib_bucket) || !safe_atom(profile.gpu_family) ||
                !safe_atom(profile.power_mode)) {
                return std::unexpected{error(
                    DiagnosticEvaluationArtifactErrorCode::invalid_format,
                    "hardware distribution contains an unsafe value")};
            }
            if (!first_profile) output << ',';
            first_profile = false;
            output << "{\"profile_id\":\"" << profile.profile_id
                   << "\",\"os_family\":\"" << profile.os_family
                   << "\",\"os_build_bucket\":\"" << profile.os_build_bucket
                   << "\",\"cpu_family\":\"" << profile.cpu_family
                   << "\",\"logical_processors\":" << profile.logical_processors
                   << ",\"memory_gib_bucket\":\"" << profile.memory_gib_bucket
                   << "\",\"gpu_family\":\"" << profile.gpu_family
                   << "\",\"power_mode\":\"" << profile.power_mode << "\"}";
        }
        output << "],\n  \"metrics\":{\n";
        write_rate(output, "supported_diagnosis_recall",
                   report.supported_diagnosis_recall);
        write_rate(output, "supported_diagnosis_precision",
                   report.supported_diagnosis_precision);
        write_rate(output, "unknown_truth_abstention",
                   report.unknown_truth_abstention);
        write_rate(output, "top1_contributor", report.top1_contributor_accuracy);
        write_rate(output, "top3_contributor", report.top3_contributor_accuracy);
        write_rate(output, "context_accuracy", report.context_accuracy);
        write_rate(output, "automatic_detection_recall",
                   report.automatic_detection_recall);
        write_rate(output, "miss_rate", report.automatic_detection_miss_rate);
        write_rate(output, "usefulness", report.usefulness);
        write_rate(output, "unknown_rate", report.unknown_rate);
        write_rate(output, "false_assertion_rate", report.false_assertion_rate);
        output << "    \"false_captures_per_hour\":{\"automatic_captures\":"
               << report.quiet_automatic_captures << ",\"exposure_hours\":"
               << report.quiet_exposure_hours << ",\"rate\":"
               << report.false_captures_per_hour << "},\n"
               << "    \"brier_score\":{\"eligible\":"
               << report.calibration_rows << ",\"value\":" << report.brier_score
               << "},\n    \"ece\":{\"eligible\":" << report.calibration_rows
               << ",\"value\":" << report.expected_calibration_error
               << ",\"bins\":[";
        for (std::size_t index = 0U; index < report.calibration_bins.size(); ++index) {
            if (index != 0U) output << ',';
            const auto& bin = report.calibration_bins[index];
            output << "{\"index\":" << index << ",\"count\":" << bin.count
                   << ",\"average_confidence\":" << bin.average_confidence
                   << ",\"accuracy\":" << bin.accuracy << '}';
        }
        output << "]},\n    \"recurrence_pair_f1\":{\"true_pairs\":"
               << report.recurrence_true_pairs << ",\"predicted_pairs\":"
               << report.recurrence_predicted_pairs
               << ",\"true_positive_pairs\":"
               << report.recurrence_true_positive_pairs << ",\"precision\":"
               << report.recurrence_pair_precision << ",\"recall\":"
               << report.recurrence_pair_recall << ",\"rate\":"
               << report.recurrence_pair_f1
               << "}\n  },\n  \"qualification_passed\":"
               << (qualify_v0151(report).passed ? "true" : "false")
               << ",\n  \"limitations\":["
                  "\"uncertain and disputed truth is excluded from primary metrics\","
                  "\"confidence metrics cover emitted supported-resource diagnoses only\","
                  "\"temporal contributor agreement is not causal proof\","
                  "\"hardware and symptom counts must accompany every published rate\""
                  "]\n}\n";
        if (!output) {
            return std::unexpected{error(
                DiagnosticEvaluationArtifactErrorCode::io,
                "cannot serialize diagnostic evaluation JSON")};
        }
        auto result = output.str();
        if (result.size() > maximum_diagnostic_evaluation_json_bytes) {
            return std::unexpected{error(
                DiagnosticEvaluationArtifactErrorCode::limit_exceeded,
                "diagnostic evaluation JSON exceeds its byte bound")};
        }
        return result;
    } catch (const std::exception& exception) {
        return std::unexpected{error(DiagnosticEvaluationArtifactErrorCode::io,
                                     exception.what())};
    } catch (...) {
        return std::unexpected{error(DiagnosticEvaluationArtifactErrorCode::io,
                                     "unknown evaluation serialization failure")};
    }
}

std::expected<std::string, DiagnosticEvaluationArtifactError>
serialize_diagnostic_predictions_tsv(
    const std::vector<DiagnosticPrediction>& predictions) noexcept {
    try {
        std::ostringstream output;
        output << prediction_header << '\n' << std::setprecision(17);
        for (const auto& prediction : predictions) {
            if (!valid_prediction(prediction)) {
                return std::unexpected{error(
                    DiagnosticEvaluationArtifactErrorCode::invalid_format,
                    "prediction cannot be represented by direct V1")};
            }
            output << prediction.incident_key << '\t'
                   << to_string(prediction.diagnosis) << '\t'
                   << prediction.confidence << '\t'
                   << to_string(prediction.context) << '\t'
                   << (prediction.automatic_capture ? 1 : 0) << '\t'
                   << prediction.recurrence_cluster << '\t'
                   << (prediction.observed_pressure
                           ? resource_name(*prediction.observed_pressure)
                           : "none")
                   << '\t' << prediction.practical_pressure_score << '\t'
                   << prediction.raw_statistical_score << '\t';
            for (std::size_t index = 0U;
                 index < prediction.contributor_ordinals.size(); ++index) {
                if (index != 0U) output << ',';
                output << prediction.contributor_ordinals[index];
            }
            output << '\n';
        }
        auto result = output.str();
        if (result.size() > maximum_diagnostic_predictions_bytes) {
            return std::unexpected{error(
                DiagnosticEvaluationArtifactErrorCode::limit_exceeded,
                "prediction table exceeds its direct-V1 byte bound")};
        }
        return result;
    } catch (const std::exception& exception) {
        return std::unexpected{error(DiagnosticEvaluationArtifactErrorCode::io,
                                     exception.what())};
    } catch (...) {
        return std::unexpected{error(DiagnosticEvaluationArtifactErrorCode::io,
                                     "unknown prediction serialization failure")};
    }
}

std::expected<VerifiedDiagnosticEvaluationArtifact,
              DiagnosticEvaluationArtifactError>
verify_diagnostic_evaluation_artifact(
    const std::filesystem::path& directory,
    const DogfoodCorpus& corpus) noexcept {
    try {
        if (!std::filesystem::is_directory(directory)) {
            return std::unexpected{error(
                DiagnosticEvaluationArtifactErrorCode::invalid_format,
                "evaluation artifact directory is missing")};
        }
        auto json = read_bounded(directory / "evaluation.json",
                                 maximum_diagnostic_evaluation_json_bytes);
        if (!json) return std::unexpected{json.error()};
        auto prediction_text = read_bounded(directory / "predictions.tsv",
                                            maximum_diagnostic_predictions_bytes);
        if (!prediction_text) return std::unexpected{prediction_text.error()};
        const auto header = parse_header(*json, corpus);
        if (!header) {
            return std::unexpected{error(
                DiagnosticEvaluationArtifactErrorCode::invalid_format,
                "evaluation JSON header or provenance is invalid")};
        }
        const auto maximum_rows = static_cast<std::size_t>(std::count_if(
            corpus.incidents.begin(), corpus.incidents.end(), [&](const auto& truth) {
                return truth.split == header->split;
            }));
        auto predictions = parse_predictions(*prediction_text, maximum_rows);
        if (!predictions) return std::unexpected{predictions.error()};
        auto report = evaluate_diagnostics(corpus, *predictions, header->split);
        if (!report) {
            return std::unexpected{error(
                DiagnosticEvaluationArtifactErrorCode::evaluation_failed,
                "published predictions cannot be evaluated against frozen truth")};
        }
        auto canonical_predictions = serialize_diagnostic_predictions_tsv(*predictions);
        auto canonical_json = serialize_diagnostic_evaluation_json(
            corpus, *report, header->metadata);
        if (!canonical_predictions || !canonical_json) {
            return std::unexpected{error(
                DiagnosticEvaluationArtifactErrorCode::evaluation_failed,
                "published evaluation cannot be regenerated canonically")};
        }
        if (*canonical_predictions != *prediction_text || *canonical_json != *json) {
            return std::unexpected{error(
                DiagnosticEvaluationArtifactErrorCode::content_mismatch,
                "evaluation artifacts do not match recomputed direct-V1 content")};
        }
        return VerifiedDiagnosticEvaluationArtifact{
            header->split, header->metadata, *report, predictions->size(),
            qualify_v0151(*report).passed};
    } catch (const std::exception& exception) {
        return std::unexpected{error(DiagnosticEvaluationArtifactErrorCode::io,
                                     exception.what())};
    } catch (...) {
        return std::unexpected{error(DiagnosticEvaluationArtifactErrorCode::io,
                                     "unknown evaluation verification failure")};
    }
}

} // namespace blackbox::evaluation
