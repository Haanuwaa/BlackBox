#include "evaluation/dogfood_corpus.hpp"
#include "evaluation/strict_number_parser.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

namespace blackbox::evaluation {
namespace {

using namespace std::string_view_literals;

constexpr std::string_view manifest_header{"blackbox-dogfood-corpus"};
constexpr std::string_view hardware_header{
    "profile_id\tos_family\tos_build_bucket\tcpu_family\tlogical_processors\t"
    "memory_gib_bucket\tgpu_family\tpower_mode"};
constexpr std::string_view sessions_header{
    "session_id\thardware_profile_id\toperator_id\tsplit\tkind\tsymptom\tduration_seconds\t"
    "expected_incidents\tautomatic_captures\tconsent_attested"};
constexpr std::string_view incidents_header{
    "incident_key\tsession_id\tsplit\tsymptom\tcertainty\tuser_visible\t"
    "expected_diagnosis\texpected_contributor_ordinal\texpected_context\t"
    "recurrence_family\tdetector_should_capture\tusefulness\tannotator_count\t"
    "disagreement"};
constexpr std::string_view annotations_header{
    "incident_key\tannotator_id\tsymptom\tcertainty\tuser_visible\t"
    "expected_diagnosis\texpected_contributor_ordinal\texpected_context\t"
    "recurrence_family\tusefulness"};

[[nodiscard]] DogfoodCorpusError error(const DogfoodCorpusErrorCode code,
                                       std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] bool safe_token(const std::string_view value,
                              const std::size_t maximum = 64U) noexcept {
    if (value.empty() || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' || character == '_' ||
               character == '.';
    });
}

[[nodiscard]] bool valid_incident_key(const std::string_view value) noexcept {
    return value.size() == 32U &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isdigit(character) != 0 ||
                      (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] std::vector<std::string_view> columns(const std::string& line) {
    std::vector<std::string_view> result;
    std::string_view remaining{line};
    while (true) {
        const auto separator = remaining.find('\t');
        result.push_back(remaining.substr(0U, separator));
        if (separator == std::string_view::npos) break;
        remaining.remove_prefix(separator + 1U);
    }
    return result;
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> integer(const std::string_view text) noexcept {
    Integer value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<double> finite_double(const std::string_view text) {
    return parse_finite_decimal(text);
}

[[nodiscard]] std::optional<bool> boolean(const std::string_view text) noexcept {
    if (text == "0") return false;
    if (text == "1") return true;
    return std::nullopt;
}

template <typename Value, std::size_t Size>
[[nodiscard]] std::optional<Value> named_value(
    const std::string_view text,
    const std::array<std::pair<std::string_view, Value>, Size>& names) noexcept {
    const auto found = std::find_if(names.begin(), names.end(), [&](const auto& entry) {
        return entry.first == text;
    });
    return found == names.end() ? std::nullopt : std::optional<Value>{found->second};
}

constexpr std::array split_names{
    std::pair{"development"sv, CorpusSplit::development},
    std::pair{"calibration"sv, CorpusSplit::calibration},
    std::pair{"held_out"sv, CorpusSplit::held_out},
};
constexpr std::array session_kind_names{
    std::pair{"controlled"sv, DogfoodSessionKind::controlled},
    std::pair{"natural"sv, DogfoodSessionKind::natural},
    std::pair{"quiet"sv, DogfoodSessionKind::quiet},
};
constexpr std::array symptom_names{
    std::pair{"cpu_starvation"sv, SymptomClass::cpu_starvation},
    std::pair{"disk_stall"sv, SymptomClass::disk_stall},
    std::pair{"network_interruption"sv, SymptomClass::network_interruption},
    std::pair{"application_crash"sv, SymptomClass::application_crash},
    std::pair{"application_hang"sv, SymptomClass::application_hang},
    std::pair{"game_stutter"sv, SymptomClass::game_stutter},
    std::pair{"audio_interruption"sv, SymptomClass::audio_interruption},
    std::pair{"quiet"sv, SymptomClass::quiet},
    std::pair{"ambiguous"sv, SymptomClass::ambiguous},
};
static_assert(symptom_names.size() == dogfood_symptom_class_count);
constexpr std::array certainty_names{
    std::pair{"confirmed"sv, TruthCertainty::confirmed},
    std::pair{"probable"sv, TruthCertainty::probable},
    std::pair{"uncertain"sv, TruthCertainty::uncertain},
    std::pair{"unresolvable"sv, TruthCertainty::unresolvable},
};
constexpr std::array usefulness_names{
    std::pair{"unscored"sv, UsefulnessRating::unscored},
    std::pair{"not_useful"sv, UsefulnessRating::not_useful},
    std::pair{"unsure"sv, UsefulnessRating::unsure},
    std::pair{"useful"sv, UsefulnessRating::useful},
};
constexpr std::array diagnosis_names{
    std::pair{"unknown"sv, analysis::IncidentType::unknown},
    std::pair{"cpu_pressure"sv, analysis::IncidentType::cpu_pressure},
    std::pair{"memory_pressure"sv, analysis::IncidentType::memory_pressure},
    std::pair{"storage_pressure"sv, analysis::IncidentType::storage_pressure},
    std::pair{"network_pressure"sv, analysis::IncidentType::network_pressure},
    std::pair{"multi_resource_pressure"sv,
              analysis::IncidentType::multi_resource_pressure},
    std::pair{"application_crash"sv, analysis::IncidentType::application_crash},
    std::pair{"application_hang"sv, analysis::IncidentType::application_hang},
    std::pair{"dns_resolution_timeout"sv,
              analysis::IncidentType::dns_resolution_timeout},
    std::pair{"display_driver_recovery"sv,
              analysis::IncidentType::display_driver_recovery},
    std::pair{"storage_io_retry"sv,
              analysis::IncidentType::storage_io_retry},
};
constexpr std::array context_names{
    std::pair{"unknown"sv, analysis::WorkloadContextKind::unknown},
    std::pair{"idle"sv, analysis::WorkloadContextKind::idle},
    std::pair{"gaming"sv, analysis::WorkloadContextKind::gaming},
    std::pair{"development"sv, analysis::WorkloadContextKind::development},
    std::pair{"compilation"sv, analysis::WorkloadContextKind::compilation},
    std::pair{"video_playback_or_call"sv,
              analysis::WorkloadContextKind::video_playback_or_call},
    std::pair{"heavy_download"sv, analysis::WorkloadContextKind::heavy_download},
    std::pair{"desktop"sv, analysis::WorkloadContextKind::desktop},
};

[[nodiscard]] std::expected<IncidentAnnotation, DogfoodCorpusError>
annotation_from_columns(const std::vector<std::string_view>& values) {
    if (values.size() != 10U) {
        return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                     "annotation column count mismatch")};
    }
    const auto symptom = named_value(values[2U], symptom_names);
    const auto certainty = named_value(values[3U], certainty_names);
    const auto visible = boolean(values[4U]);
    const auto diagnosis = named_value(values[5U], diagnosis_names);
    std::optional<std::size_t> contributor;
    if (!values[6U].empty()) contributor = integer<std::size_t>(values[6U]);
    const auto context = named_value(values[7U], context_names);
    const auto usefulness = named_value(values[9U], usefulness_names);
    if (!valid_incident_key(values[0U]) || !safe_token(values[1U]) ||
        !symptom || !certainty || !visible || !diagnosis ||
        (!values[6U].empty() && !contributor) ||
        (contributor && *contributor >= 8'192U) || !context ||
        values[8U].size() > 64U ||
        (!values[8U].empty() && !safe_token(values[8U])) || !usefulness) {
        return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                     "invalid annotation value")};
    }
    return IncidentAnnotation{std::string{values[0U]}, std::string{values[1U]},
        *symptom, *certainty, *visible, *diagnosis, contributor, *context,
        std::string{values[8U]}, *usefulness};
}

[[nodiscard]] std::expected<std::vector<std::string>, DogfoodCorpusError>
read_lines(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                             "cannot open " + path.string())};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    if (!input.eof()) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                                   "cannot read " + path.string())};
    return lines;
}

[[nodiscard]] std::expected<DogfoodManifest, DogfoodCorpusError>
read_manifest(const std::filesystem::path& path) {
    auto lines = read_lines(path);
    if (!lines) return std::unexpected{lines.error()};
    std::map<std::string, std::string> fields;
    for (const auto& line : *lines) {
        const auto separator = line.find('=');
        if (separator == std::string::npos || separator == 0U ||
            !fields.emplace(line.substr(0U, separator), line.substr(separator + 1U)).second) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_manifest,
                                         "malformed or duplicate manifest field")};
        }
    }
    constexpr std::array required{"format"sv, "version"sv, "corpus_id"sv, "state"sv,
                                  "pipeline_version"sv, "configuration_fingerprint"sv,
                                  "annotation_fingerprint"sv, "predeclared_metrics"sv};
    if (fields.size() != required.size() ||
        !std::all_of(required.begin(), required.end(), [&](const auto name) {
            return fields.contains(std::string{name});
        }) || fields["format"] != manifest_header ||
        fields["predeclared_metrics"] !=
            "supported_diagnosis_recall,supported_diagnosis_precision,top1_contributor,"
            "top3_contributor,unknown_rate,miss_rate,"
            "unknown_truth_abstention,false_assertion_rate,false_captures_per_hour,"
            "brier_score,ece,usefulness,context_accuracy,"
            "recurrence_pair_f1") {
        return std::unexpected{error(DogfoodCorpusErrorCode::invalid_manifest,
                                     "manifest contract does not match protocol v1")};
    }
    const auto version = integer<std::uint32_t>(fields["version"]);
    const auto pipeline = integer<std::uint32_t>(fields["pipeline_version"]);
    const auto configuration = integer<std::uint64_t>(fields["configuration_fingerprint"]);
    const auto annotation = integer<std::uint64_t>(fields["annotation_fingerprint"]);
    if (!version || *version != dogfood_protocol_version || !pipeline || *pipeline == 0U ||
        !configuration || *configuration == 0U || !annotation ||
        !safe_token(fields["corpus_id"]) ||
        (fields["state"] != "collecting" && fields["state"] != "frozen")) {
        return std::unexpected{error(DogfoodCorpusErrorCode::invalid_manifest,
                                     "invalid manifest value")};
    }
    return DogfoodManifest{fields["corpus_id"], fields["state"] == "frozen",
                           *pipeline, *configuration, *annotation};
}

void fingerprint_bytes(std::uint64_t& hash, const std::string_view value) noexcept {
    for (const auto byte : value) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1'099'511'628'211ULL;
    }
    hash ^= 0xFFU;
    hash *= 1'099'511'628'211ULL;
}

template <typename Value>
void fingerprint_integer(std::uint64_t& hash, const Value value) noexcept {
    fingerprint_bytes(hash, std::to_string(value));
}

[[nodiscard]] bool scorable_certainty(const TruthCertainty value) noexcept {
    return value == TruthCertainty::confirmed || value == TruthCertainty::probable;
}

[[nodiscard]] bool annotation_matches_truth(
    const IncidentAnnotation& annotation,
    const IncidentTruth& truth) noexcept {
    return annotation.symptom == truth.symptom &&
           annotation.certainty == truth.certainty &&
           annotation.user_visible == truth.user_visible &&
           annotation.expected_diagnosis == truth.expected_diagnosis &&
           annotation.expected_contributor_ordinal ==
               truth.expected_contributor_ordinal &&
           annotation.expected_context == truth.expected_context &&
           annotation.recurrence_family == truth.recurrence_family &&
           annotation.usefulness == truth.usefulness;
}

[[nodiscard]] bool annotation_payloads_match(
    const IncidentAnnotation& first,
    const IncidentAnnotation& second) noexcept {
    return first.symptom == second.symptom &&
           first.certainty == second.certainty &&
           first.user_visible == second.user_visible &&
           first.expected_diagnosis == second.expected_diagnosis &&
           first.expected_contributor_ordinal ==
               second.expected_contributor_ordinal &&
           first.expected_context == second.expected_context &&
           first.recurrence_family == second.recurrence_family &&
           first.usefulness == second.usefulness;
}

[[nodiscard]] DogfoodQualificationReport qualification_report_unchecked(
    const DogfoodCorpus& corpus) {
    DogfoodQualificationReport report{};
    std::map<std::string, DogfoodHardwareQualification> profiles;
    std::map<std::string, const DogfoodSession*> sessions;
    std::set<std::string> represented;
    for (const auto& session : corpus.sessions) {
        sessions.emplace(session.session_id, &session);
        represented.insert(session.hardware_profile_id);
        auto& profile = profiles[session.hardware_profile_id];
        profile.profile_id = session.hardware_profile_id;
        if (session.kind == DogfoodSessionKind::natural) {
            ++report.natural_sessions;
            if (session.split == CorpusSplit::calibration) {
                ++report.calibration_natural_sessions;
                profile.calibration_natural = true;
            } else if (session.split == CorpusSplit::held_out) {
                ++report.held_out_natural_sessions;
                profile.held_out_natural = true;
            }
        }
        if (session.kind == DogfoodSessionKind::quiet) {
            report.quiet_exposure_seconds += session.duration_seconds;
            if (session.split == CorpusSplit::calibration) {
                report.calibration_quiet_exposure_seconds += session.duration_seconds;
                profile.calibration_quiet_seconds += session.duration_seconds;
            } else if (session.split == CorpusSplit::held_out) {
                report.held_out_quiet_exposure_seconds += session.duration_seconds;
                profile.held_out_quiet_seconds += session.duration_seconds;
            }
        }
    }
    report.represented_hardware_profiles = represented.size();

    for (const auto& incident : corpus.incidents) {
        if (!scorable_certainty(incident.certainty) || incident.disagreement) continue;
        const auto session = sessions.find(incident.session_id);
        if (session == sessions.end()) continue;
        auto& profile = profiles[session->second->hardware_profile_id];
        if (incident.annotator_count < 2U &&
            incident.split != CorpusSplit::development) {
            ++report.insufficient_independent_annotation_rows;
        }
        if (incident.split == CorpusSplit::calibration) {
            report.calibration_symptom_coverage[
                static_cast<std::size_t>(incident.symptom)] = true;
            profile.calibration_scorable_truth = true;
            if (incident.expected_diagnosis != analysis::IncidentType::unknown) {
                ++report.calibration_supported_diagnoses;
            }
        } else if (incident.split == CorpusSplit::held_out) {
            report.held_out_symptom_coverage[
                static_cast<std::size_t>(incident.symptom)] = true;
            profile.held_out_scorable_truth = true;
            ++report.held_out_scorable_truth_rows;
        }
    }

    for (const auto& [id, profile] : profiles) {
        static_cast<void>(id);
        report.hardware_qualification.push_back(profile);
        if (profile.fully_qualified()) {
            ++report.fully_qualified_hardware_profiles;
        }
    }

    const auto all_covered = [](const auto& coverage) {
        return std::all_of(coverage.begin(), coverage.end(),
                           [](const bool value) { return value; });
    };
    if (report.fully_qualified_hardware_profiles <
        minimum_qualification_hardware_profiles) {
        report.unmet_requirements.emplace_back(
            "need three hardware profiles each with calibration and held-out natural sessions, "
            "one quiet hour in each split, and scorable truth in each split");
    }
    if (report.natural_sessions < minimum_natural_sessions) {
        report.unmet_requirements.emplace_back("need at least six natural sessions");
    }
    if (report.quiet_exposure_seconds < minimum_quiet_exposure_seconds) {
        report.unmet_requirements.emplace_back("need at least ten aggregate quiet exposure hours");
    }
    if (report.calibration_supported_diagnoses < minimum_calibration_diagnoses) {
        report.unmet_requirements.emplace_back(
            "need at least ten scorable calibration rows with supported diagnoses");
    }
    if (report.held_out_scorable_truth_rows < minimum_held_out_truth_rows) {
        report.unmet_requirements.emplace_back(
            "need at least ten scorable held-out truth rows");
    }
    if (!all_covered(report.calibration_symptom_coverage)) {
        report.unmet_requirements.emplace_back(
            "calibration scorable truth must cover all nine symptom classes");
    }
    if (!all_covered(report.held_out_symptom_coverage)) {
        report.unmet_requirements.emplace_back(
            "held-out scorable truth must cover all nine symptom classes");
    }
    if (report.insufficient_independent_annotation_rows != 0U) {
        report.unmet_requirements.emplace_back(
            "every scorable calibration and held-out row needs two independent annotation ballots");
    }
    return report;
}

[[nodiscard]] std::expected<void, DogfoodCorpusError>
validate_corpus(const DogfoodCorpus& corpus, const bool require_complete) {
    if (corpus.hardware_profiles.size() > maximum_hardware_profiles ||
        corpus.sessions.size() > maximum_dogfood_sessions ||
        corpus.incidents.size() > maximum_dogfood_incidents ||
        corpus.annotations.size() > maximum_dogfood_annotations) {
        return std::unexpected{error(DogfoodCorpusErrorCode::limit_exceeded,
                                     "corpus exceeds a hard row bound")};
    }
    std::set<std::string> profile_ids;
    for (const auto& profile : corpus.hardware_profiles) {
        if (!safe_token(profile.profile_id) || !safe_token(profile.os_family) ||
            !safe_token(profile.os_build_bucket) || !safe_token(profile.cpu_family) ||
            profile.logical_processors == 0U || profile.logical_processors > 4'096U ||
            !safe_token(profile.memory_gib_bucket) || !safe_token(profile.gpu_family) ||
            !safe_token(profile.power_mode)) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                         "invalid hardware profile")};
        }
        if (!profile_ids.insert(profile.profile_id).second) {
            return std::unexpected{error(DogfoodCorpusErrorCode::duplicate_id,
                                         "duplicate hardware profile")};
        }
    }
    std::map<std::string, const DogfoodSession*> sessions;
    for (const auto& session : corpus.sessions) {
        if (!safe_token(session.session_id) ||
            !profile_ids.contains(session.hardware_profile_id) ||
            !safe_token(session.operator_id) ||
            !std::isfinite(session.duration_seconds) || session.duration_seconds <= 0.0 ||
            session.duration_seconds > 604'800.0 ||
            session.expected_incidents > maximum_dogfood_incidents ||
            session.automatic_captures > session.expected_incidents) {
            return std::unexpected{error(DogfoodCorpusErrorCode::missing_reference,
                                         "invalid session or hardware reference")};
        }
        if (!session.consent_attested) {
            return std::unexpected{error(
                DogfoodCorpusErrorCode::invalid_value,
                "session " + session.session_id + " requires consent_attested=1")};
        }
        if ((session.kind == DogfoodSessionKind::quiet) !=
            (session.symptom == SymptomClass::quiet)) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                "quiet session kind and symptom must agree")};
        }
        if (!sessions.emplace(session.session_id, &session).second) {
            return std::unexpected{error(DogfoodCorpusErrorCode::duplicate_id,
                                         "duplicate session: " + session.session_id)};
        }
    }
    std::map<std::string, const IncidentTruth*> incident_rows;
    std::map<std::string, std::size_t> incident_counts_by_session;
    for (const auto& incident : corpus.incidents) {
        const auto session = sessions.find(incident.session_id);
        if (!valid_incident_key(incident.incident_key) || session == sessions.end() ||
            session->second->split != incident.split ||
            session->second->symptom != incident.symptom ||
            incident.annotator_count == 0U || incident.annotator_count > 64U ||
            incident.recurrence_family.size() > 64U ||
            (!incident.recurrence_family.empty() &&
             !safe_token(incident.recurrence_family)) ||
            (incident.expected_contributor_ordinal &&
             *incident.expected_contributor_ordinal >= 8'192U)) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                         "invalid incident truth row")};
        }
        if (!incident_rows.emplace(incident.incident_key, &incident).second) {
            return std::unexpected{error(DogfoodCorpusErrorCode::duplicate_id,
                                         "duplicate incident key: " +
                                             incident.incident_key)};
        }
        ++incident_counts_by_session[incident.session_id];
    }
    for (const auto& session : corpus.sessions) {
        if (incident_counts_by_session[session.session_id] != session.expected_incidents) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                "session " + session.session_id + " declares " +
                std::to_string(session.expected_incidents) + " incidents but contains " +
                std::to_string(incident_counts_by_session[session.session_id]))};
        }
    }

    std::map<std::string, std::size_t> annotation_counts;
    std::map<std::string, bool> annotation_disagreement;
    std::set<std::pair<std::string, std::string>> annotation_identities;
    for (const auto& annotation : corpus.annotations) {
        const auto incident = incident_rows.find(annotation.incident_key);
        if (incident == incident_rows.end() || !safe_token(annotation.annotator_id) ||
            annotation.recurrence_family.size() > 64U ||
            (!annotation.recurrence_family.empty() &&
             !safe_token(annotation.recurrence_family)) ||
            (annotation.expected_contributor_ordinal &&
             *annotation.expected_contributor_ordinal >= 8'192U)) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                         "invalid annotation ballot")};
        }
        const auto session = sessions.find(incident->second->session_id);
        if (session == sessions.end()) {
            return std::unexpected{error(DogfoodCorpusErrorCode::missing_reference,
                                         "annotation incident session is missing")};
        }
        if (annotation.annotator_id == session->second->operator_id) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                "session operator " + session->second->operator_id +
                " cannot annotate incident " + annotation.incident_key)};
        }
        if (!annotation_identities.emplace(annotation.incident_key,
                                           annotation.annotator_id).second) {
            return std::unexpected{error(DogfoodCorpusErrorCode::duplicate_id,
                "duplicate ballot for incident " + annotation.incident_key +
                " from annotator " + annotation.annotator_id)};
        }
        ++annotation_counts[annotation.incident_key];
        annotation_disagreement[annotation.incident_key] =
            annotation_disagreement[annotation.incident_key] ||
            !annotation_matches_truth(annotation, *incident->second);
    }
    for (const auto& incident : corpus.incidents) {
        if (annotation_counts[incident.incident_key] != incident.annotator_count) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                "incident " + incident.incident_key + " declares " +
                std::to_string(incident.annotator_count) + " ballots but contains " +
                std::to_string(annotation_counts[incident.incident_key]))};
        }
        if (annotation_disagreement[incident.incident_key] != incident.disagreement) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                "incident " + incident.incident_key + " declares disagreement=" +
                std::to_string(incident.disagreement ? 1 : 0) +
                " but ballots require disagreement=" +
                std::to_string(annotation_disagreement[incident.incident_key] ? 1 : 0))};
        }
    }

    if (require_complete && !qualification_report_unchecked(corpus).ready_to_freeze()) {
        return std::unexpected{error(DogfoodCorpusErrorCode::incomplete_coverage,
            "frozen corpus does not satisfy the multi-hardware qualification report")};
    }
    return {};
}

[[nodiscard]] std::expected<void, DogfoodCorpusError>
write_manifest(const std::filesystem::path& path, const DogfoodManifest& manifest) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                              "cannot write manifest")};
    output << "format=" << manifest_header << '\n'
           << "version=" << dogfood_protocol_version << '\n'
           << "corpus_id=" << manifest.corpus_id << '\n'
           << "state=" << (manifest.frozen ? "frozen" : "collecting") << '\n'
           << "pipeline_version=" << manifest.pipeline_version << '\n'
           << "configuration_fingerprint=" << manifest.configuration_fingerprint << '\n'
           << "annotation_fingerprint=" << manifest.annotation_fingerprint << '\n'
           << "predeclared_metrics=supported_diagnosis_recall,supported_diagnosis_precision,"
              "top1_contributor,top3_contributor,unknown_rate,miss_rate,"
              "unknown_truth_abstention,false_assertion_rate,"
              "false_captures_per_hour,brier_score,ece,usefulness,"
              "context_accuracy,recurrence_pair_f1\n";
    if (!output) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                              "cannot commit manifest")};
    return {};
}

[[nodiscard]] std::expected<void, DogfoodCorpusError>
write_header(const std::filesystem::path& path, const std::string_view header) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                              "cannot create corpus table")};
    output << header << '\n';
    if (!output) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                              "cannot write corpus table")};
    return {};
}

[[nodiscard]] std::expected<void, DogfoodCorpusError>
write_collecting_corpus(const std::filesystem::path& directory,
                        const DogfoodCorpus& corpus) {
    if (corpus.manifest.frozen || corpus.manifest.annotation_fingerprint != 0U) {
        return std::unexpected{error(DogfoodCorpusErrorCode::invalid_manifest,
                                     "cannot write a frozen collecting corpus")};
    }
    if (auto written = write_manifest(directory / "manifest.ini", corpus.manifest);
        !written) {
        return written;
    }
    {
        std::ofstream output(directory / "hardware.tsv", std::ios::binary);
        if (!output) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                                   "cannot write hardware table")};
        output << hardware_header << '\n';
        for (const auto& profile : corpus.hardware_profiles) {
            output << profile.profile_id << '\t' << profile.os_family << '\t'
                   << profile.os_build_bucket << '\t' << profile.cpu_family << '\t'
                   << profile.logical_processors << '\t' << profile.memory_gib_bucket
                   << '\t' << profile.gpu_family << '\t' << profile.power_mode << '\n';
        }
        if (!output) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                                   "cannot commit hardware table")};
    }
    {
        std::ofstream output(directory / "sessions.tsv", std::ios::binary);
        if (!output) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                                   "cannot write sessions table")};
        output << std::setprecision(std::numeric_limits<double>::max_digits10)
               << sessions_header << '\n';
        for (const auto& session : corpus.sessions) {
            output << session.session_id << '\t' << session.hardware_profile_id << '\t'
                   << session.operator_id << '\t' << to_string(session.split) << '\t'
                   << to_string(session.kind) << '\t' << to_string(session.symptom) << '\t'
                   << session.duration_seconds << '\t' << session.expected_incidents << '\t'
                   << session.automatic_captures << '\t'
                   << (session.consent_attested ? 1 : 0) << '\n';
        }
        if (!output) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                                   "cannot commit sessions table")};
    }
    {
        std::ofstream output(directory / "incidents.tsv", std::ios::binary);
        if (!output) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                                   "cannot write incidents table")};
        output << incidents_header << '\n';
        for (const auto& incident : corpus.incidents) {
            output << incident.incident_key << '\t' << incident.session_id << '\t'
                   << to_string(incident.split) << '\t' << to_string(incident.symptom) << '\t'
                   << to_string(incident.certainty) << '\t' << (incident.user_visible ? 1 : 0)
                   << '\t' << to_string(incident.expected_diagnosis) << '\t';
            if (incident.expected_contributor_ordinal) {
                output << *incident.expected_contributor_ordinal;
            }
            output << '\t' << to_string(incident.expected_context) << '\t'
                   << incident.recurrence_family << '\t'
                   << (incident.detector_should_capture ? 1 : 0) << '\t'
                   << to_string(incident.usefulness) << '\t' << incident.annotator_count
                   << '\t' << (incident.disagreement ? 1 : 0) << '\n';
        }
        if (!output) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                                   "cannot commit incidents table")};
    }
    {
        std::ofstream output(directory / "annotations.tsv", std::ios::binary);
        if (!output) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                                   "cannot write annotations table")};
        output << annotations_header << '\n';
        for (const auto& annotation : corpus.annotations) {
            output << annotation.incident_key << '\t' << annotation.annotator_id << '\t'
                   << to_string(annotation.symptom) << '\t'
                   << to_string(annotation.certainty) << '\t'
                   << (annotation.user_visible ? 1 : 0) << '\t'
                   << to_string(annotation.expected_diagnosis) << '\t';
            if (annotation.expected_contributor_ordinal) {
                output << *annotation.expected_contributor_ordinal;
            }
            output << '\t' << to_string(annotation.expected_context) << '\t'
                   << annotation.recurrence_family << '\t'
                   << to_string(annotation.usefulness) << '\n';
        }
        if (!output) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                                   "cannot commit annotations table")};
    }
    return {};
}

[[nodiscard]] bool path_has_prefix(const std::filesystem::path& path,
                                   const std::filesystem::path& prefix) {
    auto path_part = path.begin();
    for (auto prefix_part = prefix.begin(); prefix_part != prefix.end();
         ++prefix_part, ++path_part) {
        if (path_part == path.end() || *path_part != *prefix_part) return false;
    }
    return true;
}

[[nodiscard]] std::expected<void, DogfoodCorpusError>
validate_session_packet_directory(const std::filesystem::path& directory) {
    static const std::set<std::filesystem::path> expected{
        "manifest.ini", "hardware.tsv", "sessions.tsv", "incidents.tsv",
        "annotations.tsv"};
    std::set<std::filesystem::path> actual;
    std::error_code issue;
    for (std::filesystem::directory_iterator iterator{directory, issue}, end;
         !issue && iterator != end; iterator.increment(issue)) {
        const auto status = iterator->symlink_status(issue);
        if (issue || !std::filesystem::is_regular_file(status)) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                         "session packet contains a non-regular entry")};
        }
        actual.emplace(iterator->path().filename());
    }
    if (issue) return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                             "cannot enumerate session packet")};
    if (actual != expected) {
        return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                     "session packet must contain exactly five corpus files")};
    }
    return {};
}

} // namespace

std::expected<DogfoodCorpus, DogfoodCorpusError>
load_dogfood_corpus(const std::filesystem::path& directory) noexcept {
    try {
        DogfoodCorpus corpus{};
        auto manifest = read_manifest(directory / "manifest.ini");
        if (!manifest) return std::unexpected{manifest.error()};
        corpus.manifest = std::move(*manifest);

        auto hardware = read_lines(directory / "hardware.tsv");
        auto sessions = read_lines(directory / "sessions.tsv");
        auto incidents = read_lines(directory / "incidents.tsv");
        auto annotations = read_lines(directory / "annotations.tsv");
        if (!hardware) return std::unexpected{hardware.error()};
        if (!sessions) return std::unexpected{sessions.error()};
        if (!incidents) return std::unexpected{incidents.error()};
        if (!annotations) return std::unexpected{annotations.error()};
        if (hardware->empty() || hardware->front() != hardware_header ||
            sessions->empty() || sessions->front() != sessions_header ||
            incidents->empty() || incidents->front() != incidents_header ||
            annotations->empty() || annotations->front() != annotations_header) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_header,
                                         "corpus table header mismatch")};
        }

        for (std::size_t row = 1U; row < hardware->size(); ++row) {
            if ((*hardware)[row].empty()) continue;
            const auto values = columns((*hardware)[row]);
            if (values.size() != 8U) return std::unexpected{error(
                DogfoodCorpusErrorCode::invalid_value, "hardware column count mismatch")};
            const auto logical = integer<std::size_t>(values[4U]);
            if (!logical) return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                                       "invalid logical processor count")};
            corpus.hardware_profiles.push_back({std::string{values[0U]}, std::string{values[1U]},
                std::string{values[2U]}, std::string{values[3U]}, *logical,
                std::string{values[5U]}, std::string{values[6U]}, std::string{values[7U]}});
        }
        for (std::size_t row = 1U; row < sessions->size(); ++row) {
            if ((*sessions)[row].empty()) continue;
            const auto values = columns((*sessions)[row]);
            if (values.size() != 10U) return std::unexpected{error(
                DogfoodCorpusErrorCode::invalid_value, "session column count mismatch")};
            const auto split = named_value(values[3U], split_names);
            const auto kind = named_value(values[4U], session_kind_names);
            const auto symptom = named_value(values[5U], symptom_names);
            const auto duration = finite_double(values[6U]);
            const auto expected = integer<std::size_t>(values[7U]);
            const auto captures = integer<std::size_t>(values[8U]);
            const auto consent = boolean(values[9U]);
            if (!split || !kind || !symptom || !duration || !expected || !captures ||
                !consent) {
                return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                             "invalid session value")};
            }
            corpus.sessions.push_back({std::string{values[0U]}, std::string{values[1U]},
                                       std::string{values[2U]}, *split, *kind, *symptom,
                                       *duration, *expected, *captures, *consent});
        }
        for (std::size_t row = 1U; row < incidents->size(); ++row) {
            if ((*incidents)[row].empty()) continue;
            const auto values = columns((*incidents)[row]);
            if (values.size() != 14U) return std::unexpected{error(
                DogfoodCorpusErrorCode::invalid_value, "incident column count mismatch")};
            const auto split = named_value(values[2U], split_names);
            const auto symptom = named_value(values[3U], symptom_names);
            const auto certainty = named_value(values[4U], certainty_names);
            const auto visible = boolean(values[5U]);
            const auto diagnosis = named_value(values[6U], diagnosis_names);
            std::optional<std::size_t> contributor;
            if (!values[7U].empty()) contributor = integer<std::size_t>(values[7U]);
            const auto context = named_value(values[8U], context_names);
            const auto detector = boolean(values[10U]);
            const auto usefulness = named_value(values[11U], usefulness_names);
            const auto annotators = integer<std::size_t>(values[12U]);
            const auto disagreement = boolean(values[13U]);
            if (!split || !symptom || !certainty || !visible || !diagnosis ||
                (!values[7U].empty() && !contributor) || !context || !detector ||
                !usefulness || !annotators || !disagreement) {
                return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                             "invalid incident value")};
            }
            corpus.incidents.push_back({std::string{values[0U]}, std::string{values[1U]},
                *split, *symptom, *certainty, *visible, *diagnosis, contributor, *context,
                std::string{values[9U]}, *detector, *usefulness, *annotators, *disagreement});
        }
        for (std::size_t row = 1U; row < annotations->size(); ++row) {
            if ((*annotations)[row].empty()) continue;
            const auto values = columns((*annotations)[row]);
            auto annotation = annotation_from_columns(values);
            if (!annotation) return std::unexpected{annotation.error()};
            corpus.annotations.push_back(std::move(*annotation));
        }
        if (auto valid = validate_corpus(corpus, corpus.manifest.frozen); !valid) {
            return std::unexpected{valid.error()};
        }
        const auto fingerprint = dogfood_annotation_fingerprint(corpus);
        if (corpus.manifest.frozen && corpus.manifest.annotation_fingerprint != fingerprint) {
            return std::unexpected{error(DogfoodCorpusErrorCode::fingerprint_mismatch,
                                         "frozen annotation fingerprint mismatch")};
        }
        if (!corpus.manifest.frozen && corpus.manifest.annotation_fingerprint != 0U) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_manifest,
                                         "collecting corpus must have zero annotation fingerprint")};
        }
        return corpus;
    } catch (const std::exception& exception) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                     "unknown corpus read failure")};
    }
}

std::expected<IncidentAnnotation, DogfoodCorpusError> load_dogfood_annotation_ballot(
    const std::filesystem::path& ballot_path,
    const std::string_view expected_incident_key,
    const std::string_view session_operator_id) noexcept {
    try {
        if (!valid_incident_key(expected_incident_key) ||
            !safe_token(session_operator_id)) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                         "invalid ballot binding")};
        }
        std::error_code issue;
        const auto status = std::filesystem::symlink_status(ballot_path, issue);
        if (issue || !std::filesystem::is_regular_file(status)) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                         "ballot must be an existing regular file")};
        }
        const auto ballot_size = std::filesystem::file_size(ballot_path, issue);
        if (issue || ballot_size == 0U ||
            ballot_size > maximum_annotation_ballot_bytes) {
            return std::unexpected{error(DogfoodCorpusErrorCode::limit_exceeded,
                                         "ballot exceeds the exact input bound")};
        }
        auto lines = read_lines(ballot_path);
        if (!lines) return std::unexpected{lines.error()};
        if (lines->size() != 2U || lines->front() != annotations_header ||
            lines->back().empty()) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_header,
                "completed ballot must contain the exact header and one row")};
        }
        const auto values = columns(lines->back());
        if (values.size() != 10U || values[0U] != expected_incident_key) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                         "ballot incident binding or column count mismatch")};
        }
        auto annotation = annotation_from_columns(values);
        if (!annotation || annotation->annotator_id == session_operator_id) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                         "invalid completed annotation ballot")};
        }
        return *annotation;
    } catch (const std::exception& exception) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                     "unknown ballot validation failure")};
    }
}

std::expected<DogfoodBallotComparison, DogfoodCorpusError>
compare_dogfood_annotation_ballots(
    const std::filesystem::path& first_ballot_path,
    const std::filesystem::path& second_ballot_path,
    const std::string_view expected_incident_key,
    const std::string_view session_operator_id) noexcept {
    try {
        auto first = load_dogfood_annotation_ballot(
            first_ballot_path, expected_incident_key, session_operator_id);
        if (!first) return std::unexpected{first.error()};
        auto second = load_dogfood_annotation_ballot(
            second_ballot_path, expected_incident_key, session_operator_id);
        if (!second) return std::unexpected{second.error()};
        if (first->annotator_id == second->annotator_id) {
            return std::unexpected{error(DogfoodCorpusErrorCode::duplicate_id,
                                         "ballot comparison requires distinct annotators")};
        }
        return DogfoodBallotComparison{
            first->incident_key, first->annotator_id, second->annotator_id,
            !annotation_payloads_match(*first, *second)};
    } catch (const std::exception& exception) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                     "unknown ballot comparison failure")};
    }
}

std::expected<DogfoodQualificationReport, DogfoodCorpusError>
assess_dogfood_qualification(const DogfoodCorpus& corpus) noexcept {
    try {
        if (auto valid = validate_corpus(corpus, false); !valid) {
            return std::unexpected{valid.error()};
        }
        return qualification_report_unchecked(corpus);
    } catch (const std::exception& exception) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                     "unknown qualification assessment failure")};
    }
}

std::expected<std::vector<DogfoodArchiveMapEntry>, DogfoodCorpusError>
load_dogfood_archive_map(const std::filesystem::path& path,
                         const DogfoodCorpus& corpus) noexcept {
    try {
        if (auto valid = validate_corpus(corpus, false); !valid) {
            return std::unexpected{valid.error()};
        }
        auto lines = read_lines(path);
        if (!lines) return std::unexpected{lines.error()};
        if (lines->empty() || lines->front() !=
                                  "hardware_profile_id\tarchive_path") {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_header,
                                         "archive map header mismatch")};
        }
        std::set<std::string> required_profiles;
        for (const auto& session : corpus.sessions) {
            required_profiles.insert(session.hardware_profile_id);
        }
        std::set<std::string> mapped_profiles;
        std::set<std::filesystem::path> mapped_paths;
        std::vector<DogfoodArchiveMapEntry> result;
        result.reserve(required_profiles.size());
        for (std::size_t row = 1U; row < lines->size(); ++row) {
            if ((*lines)[row].empty()) continue;
            const auto values = columns((*lines)[row]);
            if (values.size() != 2U || values[1U].empty() ||
                !required_profiles.contains(std::string{values[0U]}) ||
                !mapped_profiles.insert(std::string{values[0U]}).second) {
                return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                    "archive map contains an invalid, unknown, or duplicate profile")};
            }
            auto archive_path = std::filesystem::path{values[1U]};
            if (archive_path.is_relative()) archive_path = path.parent_path() / archive_path;
            archive_path = std::filesystem::absolute(archive_path).lexically_normal();
            std::error_code path_issue;
            if (!std::filesystem::is_regular_file(archive_path, path_issue) || path_issue) {
                return std::unexpected{error(DogfoodCorpusErrorCode::missing_reference,
                    "mapped archive path must be an existing regular file")};
            }
            if (!mapped_paths.insert(archive_path).second) {
                return std::unexpected{error(DogfoodCorpusErrorCode::duplicate_id,
                    "one archive path cannot represent multiple profiles")};
            }
            for (const auto& existing : result) {
                path_issue.clear();
                if (std::filesystem::equivalent(
                        archive_path, existing.archive_path, path_issue) && !path_issue) {
                    return std::unexpected{error(DogfoodCorpusErrorCode::duplicate_id,
                        "one physical archive cannot represent multiple profiles")};
                }
                if (path_issue) {
                    return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                                 "cannot compare mapped archive paths")};
                }
            }
            result.push_back({std::string{values[0U]}, std::move(archive_path)});
        }
        if (mapped_profiles != required_profiles) {
            return std::unexpected{error(DogfoodCorpusErrorCode::missing_reference,
                "archive map must contain every represented hardware profile")};
        }
        return result;
    } catch (const std::exception& exception) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                     "unknown archive map failure")};
    }
}

std::expected<void, DogfoodCorpusError>
validate_dogfood_incident_provenance(
    const DogfoodCorpus& corpus,
    const std::span<const DogfoodIncidentLocation> locations,
    const CorpusSplit split) noexcept {
    try {
        if (auto valid = validate_corpus(corpus, false); !valid) {
            return std::unexpected{valid.error()};
        }
        std::map<std::string, std::string> profile_by_session;
        for (const auto& session : corpus.sessions) {
            profile_by_session.emplace(session.session_id,
                                       session.hardware_profile_id);
        }
        std::map<std::string, std::string> required;
        for (const auto& incident : corpus.incidents) {
            if (incident.split != split) continue;
            const auto profile = profile_by_session.find(incident.session_id);
            if (profile == profile_by_session.end()) {
                return std::unexpected{error(DogfoodCorpusErrorCode::missing_reference,
                                             "incident session has no hardware profile")};
            }
            required.emplace(incident.incident_key, profile->second);
        }
        std::set<std::string> seen;
        for (const auto& location : locations) {
            if (!seen.insert(location.incident_key).second) {
                return std::unexpected{error(DogfoodCorpusErrorCode::duplicate_id,
                    "incident key appears in more than one mapped archive")};
            }
            const auto expected = required.find(location.incident_key);
            if (expected == required.end()) continue;
            if (expected->second != location.hardware_profile_id) {
                return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                    "incident archive profile does not match its session")};
            }
            required.erase(expected);
        }
        if (!required.empty()) {
            return std::unexpected{error(DogfoodCorpusErrorCode::missing_reference,
                "mapped archives are missing truth-linked incidents")};
        }
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                     "unknown incident provenance failure")};
    }
}

std::expected<void, DogfoodCorpusError> initialize_dogfood_corpus(
    const std::filesystem::path& directory, std::string corpus_id,
    const std::uint32_t pipeline_version,
    const std::uint64_t configuration_fingerprint) noexcept {
    try {
        if (!safe_token(corpus_id) || pipeline_version == 0U ||
            configuration_fingerprint == 0U) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                         "invalid corpus identity or analysis provenance")};
        }
        if (std::filesystem::exists(directory)) {
            return std::unexpected{error(DogfoodCorpusErrorCode::already_exists,
                                         "corpus destination already exists")};
        }
        if (!std::filesystem::create_directories(directory)) {
            return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                         "cannot create corpus directory")};
        }
        DogfoodManifest manifest{std::move(corpus_id), false, pipeline_version,
                                 configuration_fingerprint, 0U};
        if (auto result = write_manifest(directory / "manifest.ini", manifest); !result)
            return result;
        if (auto result = write_header(directory / "hardware.tsv", hardware_header); !result)
            return result;
        if (auto result = write_header(directory / "sessions.tsv", sessions_header); !result)
            return result;
        if (auto result = write_header(directory / "incidents.tsv", incidents_header); !result)
            return result;
        return write_header(directory / "annotations.tsv", annotations_header);
    } catch (const std::exception& exception) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                     "unknown corpus initialization failure")};
    }
}

std::expected<void, DogfoodCorpusError> merge_dogfood_session_packet(
    const std::filesystem::path& base_corpus_directory,
    const std::filesystem::path& session_packet_directory,
    const DogfoodSessionArchiveEvidence& archive_evidence,
    const std::filesystem::path& output_corpus_directory) noexcept {
    try {
        auto base = load_dogfood_corpus(base_corpus_directory);
        if (!base) return std::unexpected{base.error()};
        auto packet = load_dogfood_corpus(session_packet_directory);
        if (!packet) return std::unexpected{packet.error()};
        if (auto exact = validate_session_packet_directory(session_packet_directory);
            !exact) {
            return exact;
        }
        if (base->manifest.frozen || packet->manifest.frozen ||
            base->manifest.corpus_id != packet->manifest.corpus_id ||
            base->manifest.pipeline_version != packet->manifest.pipeline_version ||
            base->manifest.configuration_fingerprint !=
                packet->manifest.configuration_fingerprint) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_manifest,
                "base and packet must be collecting instances of one corpus identity")};
        }
        if (packet->hardware_profiles.size() != 1U || packet->sessions.size() != 1U) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                "session packet must contain exactly one hardware profile and one session")};
        }
        const auto& packet_profile = packet->hardware_profiles.front();
        const auto& packet_session = packet->sessions.front();
        if (packet_session.hardware_profile_id != packet_profile.profile_id ||
            !std::all_of(packet->incidents.begin(), packet->incidents.end(),
                         [&](const auto& incident) {
                             return incident.session_id == packet_session.session_id;
                         })) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                         "packet rows do not belong to its one session")};
        }
        auto expected_keys = archive_evidence.incident_keys;
        auto packet_keys = std::vector<std::string>{};
        packet_keys.reserve(packet->incidents.size());
        for (const auto& incident : packet->incidents) {
            packet_keys.push_back(incident.incident_key);
        }
        std::ranges::sort(expected_keys);
        std::ranges::sort(packet_keys);
        if (std::adjacent_find(expected_keys.begin(), expected_keys.end()) !=
                expected_keys.end() ||
            expected_keys != packet_keys ||
            archive_evidence.automatic_captures != packet_session.automatic_captures) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                "archive evidence does not exactly match packet incidents and automatic captures")};
        }

        DogfoodCorpus merged = *base;
        const auto existing_profile = std::find_if(
            merged.hardware_profiles.begin(), merged.hardware_profiles.end(),
            [&](const auto& profile) {
                return profile.profile_id == packet_profile.profile_id;
            });
        if (existing_profile == merged.hardware_profiles.end()) {
            merged.hardware_profiles.push_back(packet_profile);
        } else if (*existing_profile != packet_profile) {
            return std::unexpected{error(DogfoodCorpusErrorCode::duplicate_id,
                "packet redefines an existing hardware profile")};
        }
        merged.sessions.push_back(packet_session);
        merged.incidents.insert(merged.incidents.end(), packet->incidents.begin(),
                                packet->incidents.end());
        merged.annotations.insert(merged.annotations.end(), packet->annotations.begin(),
                                  packet->annotations.end());
        if (auto valid = validate_corpus(merged, false); !valid) {
            return valid;
        }

        if (output_corpus_directory.filename().empty()) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                         "output corpus path has no directory name")};
        }
        const auto base_path = std::filesystem::weakly_canonical(base_corpus_directory);
        const auto packet_path = std::filesystem::weakly_canonical(session_packet_directory);
        const auto output_path = std::filesystem::absolute(output_corpus_directory).lexically_normal();
        if (path_has_prefix(output_path, base_path) || path_has_prefix(output_path, packet_path)) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                         "output corpus cannot be inside an input corpus")};
        }
        auto staging_path = output_path;
        staging_path += ".partial";
        if (std::filesystem::exists(output_path) || std::filesystem::exists(staging_path)) {
            return std::unexpected{error(DogfoodCorpusErrorCode::already_exists,
                "output corpus and sibling staging directory must not exist")};
        }
        std::error_code issue;
        if (!std::filesystem::create_directories(staging_path, issue) || issue) {
            return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                         "cannot create output corpus staging directory")};
        }
        if (auto written = write_collecting_corpus(staging_path, merged); !written) {
            return written;
        }
        auto verified = load_dogfood_corpus(staging_path);
        if (!verified || *verified != merged) {
            return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                         "staged merged corpus did not verify exactly")};
        }
        std::filesystem::rename(staging_path, output_path, issue);
        if (issue) {
            return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                         "cannot publish merged corpus directory")};
        }
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{error(DogfoodCorpusErrorCode::io,
                                     "unknown session packet merge failure")};
    }
}

std::expected<std::uint64_t, DogfoodCorpusError> freeze_dogfood_corpus(
    const std::filesystem::path& directory,
    const std::filesystem::path& excluded_incidents) noexcept {
    auto corpus = load_dogfood_corpus(directory);
    if (!corpus) return std::unexpected{corpus.error()};
    if (corpus->manifest.frozen) return corpus->manifest.annotation_fingerprint;
    if (auto valid = validate_corpus(*corpus, true); !valid) {
        return std::unexpected{valid.error()};
    }
    auto excluded = read_lines(excluded_incidents);
    if (!excluded) return std::unexpected{excluded.error()};
    if (excluded->empty() || excluded->front() != incidents_header) {
        return std::unexpected{error(DogfoodCorpusErrorCode::invalid_header,
                                     "excluded incident table header mismatch")};
    }
    std::set<std::string> held_out_keys;
    for (std::size_t row = 1U; row < excluded->size(); ++row) {
        if ((*excluded)[row].empty()) continue;
        const auto values = columns((*excluded)[row]);
        if (values.size() != 14U) {
            return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
                                         "excluded incident column count mismatch")};
        }
        if (values[2U] == "held_out") held_out_keys.emplace(values[0U]);
    }
    const auto reused = std::find_if(
        corpus->incidents.begin(), corpus->incidents.end(), [&](const auto& incident) {
            return held_out_keys.contains(incident.incident_key);
        });
    if (reused != corpus->incidents.end()) {
        return std::unexpected{error(DogfoodCorpusErrorCode::invalid_value,
            "candidate corpus reuses an excluded held-out incident key")};
    }
    corpus->manifest.frozen = true;
    corpus->manifest.annotation_fingerprint = dogfood_annotation_fingerprint(*corpus);
    if (auto written = write_manifest(directory / "manifest.ini", corpus->manifest); !written)
        return std::unexpected{written.error()};
    return corpus->manifest.annotation_fingerprint;
}

std::uint64_t dogfood_annotation_fingerprint(const DogfoodCorpus& corpus) noexcept {
    std::uint64_t hash{1'469'598'103'934'665'603ULL};
    fingerprint_bytes(hash, corpus.manifest.corpus_id);
    fingerprint_integer(hash, corpus.manifest.pipeline_version);
    fingerprint_integer(hash, corpus.manifest.configuration_fingerprint);
    for (const auto& profile : corpus.hardware_profiles) {
        fingerprint_bytes(hash, profile.profile_id); fingerprint_bytes(hash, profile.os_family);
        fingerprint_bytes(hash, profile.os_build_bucket); fingerprint_bytes(hash, profile.cpu_family);
        fingerprint_integer(hash, profile.logical_processors);
        fingerprint_bytes(hash, profile.memory_gib_bucket); fingerprint_bytes(hash, profile.gpu_family);
        fingerprint_bytes(hash, profile.power_mode);
    }
    for (const auto& session : corpus.sessions) {
        fingerprint_bytes(hash, session.session_id); fingerprint_bytes(hash, session.hardware_profile_id);
        fingerprint_bytes(hash, session.operator_id);
        fingerprint_integer(hash, static_cast<unsigned>(session.split));
        fingerprint_integer(hash, static_cast<unsigned>(session.kind));
        fingerprint_integer(hash, static_cast<unsigned>(session.symptom));
        fingerprint_bytes(hash, std::to_string(session.duration_seconds));
        fingerprint_integer(hash, session.expected_incidents);
        fingerprint_integer(hash, session.automatic_captures);
        fingerprint_integer(hash, session.consent_attested);
    }
    for (const auto& incident : corpus.incidents) {
        fingerprint_bytes(hash, incident.incident_key); fingerprint_bytes(hash, incident.session_id);
        fingerprint_integer(hash, static_cast<unsigned>(incident.split));
        fingerprint_integer(hash, static_cast<unsigned>(incident.symptom));
        fingerprint_integer(hash, static_cast<unsigned>(incident.certainty));
        fingerprint_integer(hash, incident.user_visible); fingerprint_integer(hash, static_cast<unsigned>(incident.expected_diagnosis));
        fingerprint_bytes(hash, incident.expected_contributor_ordinal
                                   ? std::to_string(*incident.expected_contributor_ordinal) : "");
        fingerprint_integer(hash, static_cast<unsigned>(incident.expected_context));
        fingerprint_bytes(hash, incident.recurrence_family); fingerprint_integer(hash, incident.detector_should_capture);
        fingerprint_integer(hash, static_cast<unsigned>(incident.usefulness));
        fingerprint_integer(hash, incident.annotator_count); fingerprint_integer(hash, incident.disagreement);
    }
    for (const auto& annotation : corpus.annotations) {
        fingerprint_bytes(hash, annotation.incident_key);
        fingerprint_bytes(hash, annotation.annotator_id);
        fingerprint_integer(hash, static_cast<unsigned>(annotation.symptom));
        fingerprint_integer(hash, static_cast<unsigned>(annotation.certainty));
        fingerprint_integer(hash, annotation.user_visible);
        fingerprint_integer(hash, static_cast<unsigned>(annotation.expected_diagnosis));
        fingerprint_bytes(hash, annotation.expected_contributor_ordinal
                                   ? std::to_string(*annotation.expected_contributor_ordinal) : "");
        fingerprint_integer(hash, static_cast<unsigned>(annotation.expected_context));
        fingerprint_bytes(hash, annotation.recurrence_family);
        fingerprint_integer(hash, static_cast<unsigned>(annotation.usefulness));
    }
    return hash;
}

const char* to_string(const CorpusSplit value) noexcept {
    return split_names[static_cast<std::size_t>(value)].first.data();
}
const char* to_string(const DogfoodSessionKind value) noexcept {
    return session_kind_names[static_cast<std::size_t>(value)].first.data();
}
const char* to_string(const SymptomClass value) noexcept {
    return symptom_names[static_cast<std::size_t>(value)].first.data();
}
const char* to_string(const TruthCertainty value) noexcept {
    return certainty_names[static_cast<std::size_t>(value)].first.data();
}
const char* to_string(const UsefulnessRating value) noexcept {
    return usefulness_names[static_cast<std::size_t>(value)].first.data();
}
const char* to_string(const analysis::IncidentType value) noexcept {
    return diagnosis_names[static_cast<std::size_t>(value)].first.data();
}
const char* to_string(const analysis::WorkloadContextKind value) noexcept {
    return context_names[static_cast<std::size_t>(value)].first.data();
}

} // namespace blackbox::evaluation
