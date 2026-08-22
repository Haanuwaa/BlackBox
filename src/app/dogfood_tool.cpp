#include "analysis/incident_clustering.hpp"
#include "analysis/intelligent_incident_analyzer.hpp"
#include "evaluation/campaign_status.hpp"
#include "evaluation/confidence_calibration_artifact.hpp"
#include "evaluation/diagnostic_evaluation.hpp"
#include "evaluation/diagnostic_evaluation_artifact.hpp"
#include "evaluation/dogfood_corpus.hpp"
#include "evaluation/evaluation_run_transaction.hpp"
#include "evaluation/truth_review.hpp"
#include "storage/incident_archive.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace analysis = blackbox::analysis;
namespace evaluation = blackbox::evaluation;
namespace storage = blackbox::storage;
namespace core = blackbox::core;

namespace {

struct PipelineIdentity {
    std::uint32_t version{};
    std::uint64_t configuration_fingerprint{};
};

[[nodiscard]] PipelineIdentity current_pipeline_identity() {
    analysis::IntelligentIncidentAnalyzer analyzer;
    return {analyzer.pipeline_version(),
            analysis::intelligent_configuration_fingerprint(analyzer.configuration())};
}

[[nodiscard]] std::string export_key_text(const storage::IncidentExportKey& key) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(key.bytes.size() * 2U, '0');
    for (std::size_t index = 0U; index < key.bytes.size(); ++index) {
        result[index * 2U] = digits[key.bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[key.bytes[index] & 0x0FU];
    }
    return result;
}

[[nodiscard]] std::optional<evaluation::CorpusSplit> parse_split(
    const std::string_view text) noexcept {
    if (text == "development") return evaluation::CorpusSplit::development;
    if (text == "calibration") return evaluation::CorpusSplit::calibration;
    if (text == "held_out") return evaluation::CorpusSplit::held_out;
    return std::nullopt;
}

[[nodiscard]] const char* resource_name(const analysis::ResourceKind value) noexcept {
    switch (value) {
    case analysis::ResourceKind::cpu: return "cpu";
    case analysis::ResourceKind::memory: return "memory";
    case analysis::ResourceKind::disk: return "disk";
    case analysis::ResourceKind::network: return "network";
    }
    return "unknown";
}

void print_readiness(const evaluation::DogfoodQualificationReport& report) {
    std::cout << std::setprecision(12)
              << "qualification_ready=" << (report.ready_to_freeze() ? 1 : 0) << '\n'
              << "represented_hardware_profiles="
              << report.represented_hardware_profiles << '\n'
              << "fully_qualified_hardware_profiles="
              << report.fully_qualified_hardware_profiles << '\n'
              << "natural_sessions=" << report.natural_sessions << '\n'
              << "calibration_natural_sessions="
              << report.calibration_natural_sessions << '\n'
              << "held_out_natural_sessions="
              << report.held_out_natural_sessions << '\n'
              << "quiet_exposure_hours="
              << report.quiet_exposure_seconds / 3'600.0 << '\n'
              << "calibration_quiet_exposure_hours="
              << report.calibration_quiet_exposure_seconds / 3'600.0 << '\n'
              << "held_out_quiet_exposure_hours="
              << report.held_out_quiet_exposure_seconds / 3'600.0 << '\n'
              << "calibration_supported_diagnoses="
              << report.calibration_supported_diagnoses << '\n'
              << "held_out_scorable_truth_rows="
              << report.held_out_scorable_truth_rows << '\n'
              << "insufficient_independent_annotation_rows="
              << report.insufficient_independent_annotation_rows << '\n';
    for (const auto& profile : report.hardware_qualification) {
        std::cout << "profile=" << profile.profile_id
                  << " calibration_natural=" << (profile.calibration_natural ? 1 : 0)
                  << " held_out_natural=" << (profile.held_out_natural ? 1 : 0)
                  << " calibration_quiet_hours="
                  << profile.calibration_quiet_seconds / 3'600.0
                  << " held_out_quiet_hours="
                  << profile.held_out_quiet_seconds / 3'600.0
                  << " calibration_scorable="
                  << (profile.calibration_scorable_truth ? 1 : 0)
                  << " held_out_scorable="
                  << (profile.held_out_scorable_truth ? 1 : 0)
                  << " fully_qualified=" << (profile.fully_qualified() ? 1 : 0)
                  << '\n';
    }
    for (std::size_t index = 0U;
         index < report.calibration_symptom_coverage.size(); ++index) {
        const auto symptom = static_cast<evaluation::SymptomClass>(index);
        std::cout << "symptom=" << evaluation::to_string(symptom)
                  << " calibration="
                  << (report.calibration_symptom_coverage[index] ? 1 : 0)
                  << " held_out="
                  << (report.held_out_symptom_coverage[index] ? 1 : 0) << '\n';
    }
    for (const auto& requirement : report.unmet_requirements) {
        std::cout << "unmet=" << requirement << '\n';
    }
}

[[nodiscard]] const char* held_out_state_name(
    const evaluation::HeldOutEvaluationState state) noexcept {
    switch (state) {
    case evaluation::HeldOutEvaluationState::not_started: return "not_started";
    case evaluation::HeldOutEvaluationState::running: return "running";
    case evaluation::HeldOutEvaluationState::complete: return "complete";
    }
    return "invalid";
}

int report_held_out_status(const std::filesystem::path& corpus_path) {
    auto corpus = evaluation::load_dogfood_corpus(corpus_path);
    if (!corpus) {
        std::cerr << "Corpus invalid: " << corpus.error().message << '\n';
        return 1;
    }
    auto status = evaluation::held_out_evaluation_status(corpus_path);
    if (!status) {
        std::cerr << "Held-out status invalid: " << status.error().message << '\n';
        return 1;
    }
    std::cout << "state=" << held_out_state_name(status->state) << '\n';
    if (status->state != evaluation::HeldOutEvaluationState::not_started) {
        std::cout << "annotation_fingerprint=" << status->annotation_fingerprint << '\n'
                  << "configuration_fingerprint="
                  << status->configuration_fingerprint << '\n'
                  << "calibration_artifact_fingerprint="
                  << status->calibration_artifact_fingerprint << '\n';
    }
    if (status->state == evaluation::HeldOutEvaluationState::complete) {
        std::cout << "qualification_passed="
                  << (*status->qualification_passed ? 1 : 0) << '\n'
                  << "report_artifact_fingerprint="
                  << *status->report_artifact_fingerprint << '\n';
    }
    return 0;
}

int verify_evaluation_output(const std::filesystem::path& corpus_path,
                             const std::filesystem::path& output_path,
                             const std::filesystem::path& calibration_path);

int report_corpus(const std::filesystem::path& path, const bool require_ready) {
    auto corpus = evaluation::load_dogfood_corpus(path);
    if (!corpus) {
        std::cerr << "Validation failed: " << corpus.error().message << '\n';
        return 1;
    }
    auto readiness = evaluation::assess_dogfood_qualification(*corpus);
    if (!readiness) {
        std::cerr << "Readiness assessment failed: " << readiness.error().message << '\n';
        return 1;
    }
    std::cout << "Corpus " << corpus->manifest.corpus_id << " is "
              << (corpus->manifest.frozen ? "frozen" : "collecting") << " with "
              << corpus->hardware_profiles.size() << " hardware profiles, "
              << corpus->sessions.size() << " sessions, "
              << corpus->incidents.size() << " incidents, and "
              << corpus->annotations.size() << " annotation ballots.\n";
    print_readiness(*readiness);
    return require_ready && !readiness->ready_to_freeze() ? 3 : 0;
}

int export_campaign_readiness(const std::filesystem::path& corpus_path,
                              const std::filesystem::path& output_path) {
    auto corpus = evaluation::load_dogfood_corpus(corpus_path);
    if (!corpus) {
        std::cerr << "Corpus invalid: " << corpus.error().message << '\n';
        return 1;
    }
    const auto created = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto exported = evaluation::export_campaign_status(*corpus, created, output_path);
    if (!exported) {
        std::cerr << "Campaign-status export failed: " << exported.error().message << '\n';
        return 1;
    }
    std::cout << "Published prediction-free campaign status at "
              << output_path.string() << "; qualification_ready="
              << (exported->qualification_ready ? 1 : 0) << ".\n";
    return 0;
}

int validate_annotation_ballot(const std::filesystem::path& ballot_path,
                               const std::string_view expected_incident_key,
                               const std::string_view session_operator_id) {
    auto ballot = evaluation::load_dogfood_annotation_ballot(
        ballot_path, expected_incident_key, session_operator_id);
    if (!ballot) {
        std::cerr << "Ballot invalid: " << ballot.error().message << '\n';
        return 1;
    }
    std::cout << "ballot_valid=1\n"
              << "prediction_free=1\n"
              << "incident_key=" << ballot->incident_key << '\n'
              << "annotator_id=" << ballot->annotator_id << '\n';
    return 0;
}

int compare_annotation_ballots(const std::filesystem::path& first_ballot_path,
                               const std::filesystem::path& second_ballot_path,
                               const std::string_view expected_incident_key,
                               const std::string_view session_operator_id) {
    auto comparison = evaluation::compare_dogfood_annotation_ballots(
        first_ballot_path, second_ballot_path,
        expected_incident_key, session_operator_id);
    if (!comparison) {
        std::cerr << "Ballot comparison invalid: " << comparison.error().message << '\n';
        return 1;
    }
    std::cout << "ballots_valid=1\n"
              << "prediction_free=1\n"
              << "incident_key=" << comparison->incident_key << '\n'
              << "annotator_count=2\n"
              << "annotator_a=" << comparison->first_annotator_id << '\n'
              << "annotator_b=" << comparison->second_annotator_id << '\n'
              << "disagreement=" << (comparison->disagreement ? 1 : 0) << '\n';
    return 0;
}

int initialize_session_packet(const std::filesystem::path& base_corpus_path,
                              const std::filesystem::path& packet_path,
                              const std::uint32_t pipeline_version,
                              const std::uint64_t configuration_fingerprint) {
    auto base = evaluation::load_dogfood_corpus(base_corpus_path);
    if (!base) {
        std::cerr << "Base corpus invalid: " << base.error().message << '\n';
        return 1;
    }
    if (base->manifest.frozen) {
        std::cerr << "A frozen corpus cannot accept a new session packet.\n";
        return 1;
    }
    if (base->manifest.pipeline_version != pipeline_version ||
        base->manifest.configuration_fingerprint != configuration_fingerprint) {
        std::cerr << "Base corpus analysis provenance does not match this executable.\n";
        return 1;
    }
    auto initialized = evaluation::initialize_dogfood_corpus(
        packet_path, base->manifest.corpus_id, pipeline_version,
        configuration_fingerprint);
    if (!initialized) {
        std::cerr << "Session packet initialization failed: "
                  << initialized.error().message << '\n';
        return 1;
    }
    std::cout << "Initialized one-session packet for corpus "
              << base->manifest.corpus_id << ".\n";
    return 0;
}

[[nodiscard]] std::vector<storage::StoredIncidentSummary> list_all(
    const storage::SqliteIncidentArchive& archive) {
    std::vector<storage::StoredIncidentSummary> summaries;
    std::size_t offset{};
    while (true) {
        auto page = archive.list_page(storage::IncidentListQuery{
            .offset = offset,
            .limit = storage::maximum_incident_page_size,
            .sort = storage::IncidentListSort::oldest_first});
        if (!page) throw std::runtime_error{page.error().message};
        summaries.insert(summaries.end(), page->incidents.begin(), page->incidents.end());
        offset += page->incidents.size();
        if (offset >= page->total_matching || page->incidents.empty()) break;
    }
    return summaries;
}

struct LoadedIncident {
    storage::StoredIncidentSummary summary{};
    std::shared_ptr<const core::IncidentSnapshot> snapshot{};
    storage::SqliteIncidentArchive* archive{};
};

[[nodiscard]] analysis::IncidentRecurrenceContext recurrence_context(
    const analysis::IncidentCluster& cluster) noexcept {
    analysis::IncidentRecurrenceContext result{};
    result.available = true;
    result.recurring = true;
    result.manually_overridden = cluster.manually_overridden;
    result.occurrence_count = cluster.incident_ids.size();
    result.shared_characteristic_count = cluster.shared_characteristics.size();
    result.maximum_pair_distance = cluster.maximum_pair_distance;
    if (!cluster.shared_characteristics.empty()) {
        for (const auto& characteristic : cluster.shared_characteristics) {
            result.average_shared_support += characteristic.support;
        }
        result.average_shared_support /=
            static_cast<double>(cluster.shared_characteristics.size());
    }
    return result;
}

[[nodiscard]] std::vector<analysis::ExecutableProfileObservation> profile_history(
    storage::SqliteIncidentArchive& archive, const LoadedIncident& incident) {
    std::set<std::string, std::less<>> key_set;
    for (const auto& process : incident.snapshot->process_metadata()) {
        if (const auto identity = analysis::normalize_executable_identity(process)) {
            key_set.insert(identity->key);
            if (key_set.size() == storage::maximum_process_profile_query_identities) break;
        }
    }
    std::vector<std::string> keys{key_set.begin(), key_set.end()};
    auto stored = archive.process_profile_context(incident.summary.id, keys);
    if (!stored) return {};
    std::vector<analysis::ExecutableProfileObservation> result;
    result.reserve(stored->history.size());
    for (const auto& observation : stored->history) {
        result.push_back({observation.executable_key, observation.display_name,
                          observation.incident_id, observation.observed_utc_milliseconds,
                          observation.cpu_fraction, observation.working_set_bytes,
                          observation.disk_read_bytes_per_second,
                          observation.disk_write_bytes_per_second});
    }
    return result;
}

[[nodiscard]] std::map<core::IncidentProcessIdentity, std::size_t>
process_ordinals(const core::IncidentSnapshot& incident) {
    std::map<core::IncidentProcessIdentity, std::size_t> result;
    for (const auto& sample : incident.process_samples()) {
        result.emplace(sample.identity, result.size());
    }
    return result;
}

int verify_evaluation_output(const std::filesystem::path& corpus_path,
                             const std::filesystem::path& output_path,
                             const std::filesystem::path& calibration_path) {
    auto corpus = evaluation::load_dogfood_corpus(corpus_path);
    if (!corpus) {
        std::cerr << "Corpus invalid: " << corpus.error().message << '\n';
        return 1;
    }
    if (!corpus->manifest.frozen) {
        std::cerr << "Evaluation verification requires a frozen corpus.\n";
        return 1;
    }
    auto verified = evaluation::verify_diagnostic_evaluation_artifact(
        output_path, *corpus);
    if (!verified) {
        std::cerr << "Evaluation artifact invalid: "
                  << verified.error().message << '\n';
        return 1;
    }
    std::optional<evaluation::ConfidenceCalibrationArtifact> calibration;
    std::uint64_t expected_calibration_fingerprint{};
    if (calibration_path != "none") {
        auto loaded = evaluation::load_confidence_calibration_artifact(
            calibration_path);
        if (loaded) calibration = std::move(*loaded);
        if (!calibration || calibration->annotation_fingerprint !=
                                corpus->manifest.annotation_fingerprint ||
            calibration->configuration_fingerprint !=
                corpus->manifest.configuration_fingerprint) {
            std::cerr << "Supplied calibration is invalid or has wrong provenance.\n";
            return 1;
        }
        const std::array calibration_files{calibration_path};
        const auto fingerprint = evaluation::evaluation_artifact_fingerprint(
            calibration_files);
        if (!fingerprint) {
            std::cerr << "Calibration artifact cannot be fingerprinted.\n";
            return 1;
        }
        expected_calibration_fingerprint = *fingerprint;
    }
    const evaluation::DiagnosticEvaluationArtifactMetadata expected_metadata{
        calibration.has_value(), expected_calibration_fingerprint,
        calibration && calibration->assertion.assertions_enabled,
        calibration ? calibration->assertion.threshold : 0.0};
    if (verified->metadata != expected_metadata) {
        std::cerr << "Evaluation artifact does not bind the supplied calibration.\n";
        return 1;
    }
    const std::array report_files{
        std::filesystem::path{output_path} / "evaluation.json",
        std::filesystem::path{output_path} / "predictions.tsv"};
    const auto report_fingerprint =
        evaluation::evaluation_artifact_fingerprint(report_files);
    if (!report_fingerprint) {
        std::cerr << "Evaluation artifact cannot be fingerprinted: "
                  << report_fingerprint.error().message << '\n';
        return 1;
    }
    std::cout << "artifact_valid=1\n"
              << "format="
              << evaluation::diagnostic_evaluation_report_format_version << '\n'
              << "split=" << evaluation::to_string(verified->split) << '\n'
              << "prediction_rows=" << verified->prediction_rows << '\n'
              << "truth_rows=" << verified->report.truth_rows << '\n'
              << "qualification_passed="
              << (verified->qualification_passed ? 1 : 0) << '\n'
              << "calibration_artifact_fingerprint="
              << verified->metadata.calibration_artifact_fingerprint << '\n'
              << "report_artifact_fingerprint=" << *report_fingerprint << '\n'
              << "annotation_fingerprint="
              << corpus->manifest.annotation_fingerprint << '\n'
              << "configuration_fingerprint="
              << corpus->manifest.configuration_fingerprint << '\n';
    return 0;
}

[[nodiscard]] bool write_report(const std::filesystem::path& directory,
                                const evaluation::DogfoodCorpus& corpus,
                                const evaluation::DiagnosticEvaluationReport& report,
                                const std::vector<evaluation::DiagnosticPrediction>& predictions,
                                 const std::optional<evaluation::ConfidenceCalibrationArtifact>& calibration,
                                const std::uint64_t calibration_artifact_fingerprint) {
    const evaluation::DiagnosticEvaluationArtifactMetadata metadata{
        calibration.has_value(),
        calibration_artifact_fingerprint,
        calibration && calibration->assertion.assertions_enabled,
        calibration ? calibration->assertion.threshold : 0.0};
    const auto json = evaluation::serialize_diagnostic_evaluation_json(
        corpus, report, metadata);
    const auto tsv = evaluation::serialize_diagnostic_predictions_tsv(predictions);
    if (!json || !tsv) return false;
    std::ofstream output(directory / "evaluation.json", std::ios::binary | std::ios::trunc);
    std::ofstream rows(directory / "predictions.tsv", std::ios::binary | std::ios::trunc);
    if (!output || !rows) return false;
    output.write(json->data(), static_cast<std::streamsize>(json->size()));
    rows.write(tsv->data(), static_cast<std::streamsize>(tsv->size()));
    return static_cast<bool>(output) && static_cast<bool>(rows);
}

int evaluate(const std::filesystem::path& archive_map_path,
             const std::filesystem::path& corpus_path,
             const evaluation::CorpusSplit split,
             const std::filesystem::path& calibration_path,
             const std::filesystem::path& output_path) {
    auto corpus = evaluation::load_dogfood_corpus(corpus_path);
    if (!corpus) {
        std::cerr << "Corpus invalid: " << corpus.error().message << '\n';
        return 1;
    }
    if (!corpus->manifest.frozen) {
        std::cerr << "Evaluation requires a frozen corpus.\n";
        return 1;
    }
    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto configuration = analysis::intelligent_configuration_fingerprint(
        analyzer.configuration());
    if (corpus->manifest.pipeline_version != analyzer.pipeline_version() ||
        corpus->manifest.configuration_fingerprint != configuration) {
        std::cerr << "Corpus analysis provenance does not match this executable.\n";
        return 1;
    }
    std::optional<evaluation::ConfidenceCalibrationArtifact> calibration;
    if (calibration_path != "none") {
        auto loaded = evaluation::load_confidence_calibration_artifact(
            calibration_path);
        if (loaded) calibration = std::move(*loaded);
        if (!calibration || calibration->annotation_fingerprint !=
                                corpus->manifest.annotation_fingerprint ||
            calibration->configuration_fingerprint != configuration) {
            std::cerr << "Calibration artifact is invalid or belongs to another corpus/configuration.\n";
            return 1;
        }
    }
    if (split == evaluation::CorpusSplit::held_out && !calibration) {
        std::cerr << "Held-out evaluation requires the frozen calibration artifact.\n";
        return 1;
    }
    if (auto destination = evaluation::validate_evaluation_output_destination(
            output_path); !destination) {
        std::cerr << "Output destination invalid: "
                  << destination.error().message << '\n';
        return 1;
    }
    std::optional<std::uint64_t> calibration_artifact_fingerprint;
    if (calibration) {
        const std::array calibration_files{calibration_path};
        auto fingerprint = evaluation::evaluation_artifact_fingerprint(
            calibration_files);
        if (!fingerprint) {
            std::cerr << "Calibration artifact cannot be fingerprinted: "
                      << fingerprint.error().message << '\n';
            return 1;
        }
        calibration_artifact_fingerprint = *fingerprint;
    }
    auto archive_map = evaluation::load_dogfood_archive_map(
        archive_map_path, *corpus);
    if (!archive_map) {
        std::cerr << "Archive map invalid: " << archive_map.error().message << '\n';
        return 1;
    }
    struct ArchiveSource {
        std::string hardware_profile_id{};
        std::unique_ptr<storage::SqliteIncidentArchive> archive{};
    };
    struct LocatedIncident {
        storage::StoredIncidentSummary summary{};
        storage::SqliteIncidentArchive* archive{};
    };
    std::vector<ArchiveSource> archives;
    archives.reserve(archive_map->size());
    std::map<std::string, LocatedIncident> summaries;
    std::vector<evaluation::DogfoodIncidentLocation> locations;
    try {
        for (const auto& entry : *archive_map) {
            if (!std::filesystem::exists(entry.archive_path)) {
                std::cerr << "Archive does not exist for profile "
                          << entry.hardware_profile_id << ".\n";
                return 1;
            }
            auto archive = std::make_unique<storage::SqliteIncidentArchive>(
                storage::ArchiveConfiguration{
                    .path = entry.archive_path,
                    .open_mode = storage::ArchiveOpenMode::read_only});
            if (auto opened = archive->open(); !opened) {
                std::cerr << "Archive open failed for profile "
                          << entry.hardware_profile_id << ": "
                          << opened.error().message << '\n';
                return 1;
            }
            auto* archive_pointer = archive.get();
            for (auto& summary : list_all(*archive_pointer)) {
                const auto key = export_key_text(summary.export_key);
                locations.push_back({key, entry.hardware_profile_id});
                if (!summaries.emplace(key, LocatedIncident{
                        std::move(summary), archive_pointer}).second) {
                    std::cerr << "Incident key appears in more than one mapped archive.\n";
                    return 1;
                }
            }
            archives.push_back({entry.hardware_profile_id, std::move(archive)});
        }
    } catch (const std::exception& exception) {
        std::cerr << "Archive listing failed: " << exception.what() << '\n';
        return 1;
    }
    if (auto provenance = evaluation::validate_dogfood_incident_provenance(
            *corpus, locations, split); !provenance) {
        std::cerr << "Archive provenance invalid: "
                  << provenance.error().message << '\n';
        return 1;
    }
    std::vector<LoadedIncident> loaded;
    for (const auto& truth : corpus->incidents) {
        if (truth.split != split) continue;
        const auto summary = summaries.find(truth.incident_key);
        if (summary == summaries.end()) continue;
        auto snapshot = summary->second.archive->load(summary->second.summary.id);
        if (!snapshot) {
            std::cerr << "Incident load failed: " << snapshot.error().message << '\n';
            return 1;
        }
        loaded.push_back({summary->second.summary, std::move(*snapshot),
                          summary->second.archive});
    }
    auto output_transaction = evaluation::begin_evaluation_output(output_path);
    if (!output_transaction) {
        std::cerr << "Cannot begin evaluation output: "
                  << output_transaction.error().message << '\n';
        return 1;
    }
    std::optional<evaluation::HeldOutEvaluationAttempt> held_out_attempt;
    if (split == evaluation::CorpusSplit::held_out) {
        auto acquired = evaluation::acquire_held_out_evaluation_attempt(
            corpus_path, corpus->manifest.annotation_fingerprint, configuration,
            *calibration_artifact_fingerprint);
        if (!acquired) {
            std::error_code ignored;
            std::filesystem::remove(output_transaction->staging_directory, ignored);
            std::cerr << "Held-out evaluation cannot start: "
                      << acquired.error().message << '\n';
            return 1;
        }
        held_out_attempt = std::move(*acquired);
    }
    std::vector<analysis::IncidentClusterInput> cluster_inputs;
    cluster_inputs.reserve(loaded.size());
    for (const auto& incident : loaded) {
        cluster_inputs.push_back({analysis::extract_incident_features(
            incident.summary.id, incident.summary.created_utc_milliseconds,
            *incident.snapshot), {}});
    }
    const auto clusters = analysis::cluster_incidents(cluster_inputs);
    std::map<std::int64_t, analysis::IncidentRecurrenceContext> recurrence_by_id;
    std::map<std::int64_t, std::string> cluster_by_id;
    for (const auto& cluster : clusters.clusters) {
        const auto context = recurrence_context(cluster);
        const auto key = std::to_string(cluster.stable_key);
        for (const auto id : cluster.incident_ids) {
            recurrence_by_id[id] = context;
            cluster_by_id[id] = key;
        }
    }

    std::vector<evaluation::DiagnosticPrediction> predictions;
    std::vector<evaluation::CalibrationSample> calibration_samples;
    predictions.reserve(loaded.size());
    for (const auto& incident : loaded) {
        auto history = profile_history(*incident.archive, incident);
        const analysis::IncidentAnalysisContext context{
            incident.summary.id, incident.summary.created_utc_milliseconds, history,
            recurrence_by_id[incident.summary.id]};
        auto analyzed = analyzer.analyze(*incident.snapshot, context);
        if (!analyzed) {
            std::cerr << "Analysis failed: " << analyzed.error().message << '\n';
            return 1;
        }
        const auto ordinals = process_ordinals(*incident.snapshot);
        evaluation::DiagnosticPrediction prediction{};
        prediction.incident_key = export_key_text(incident.summary.export_key);
        prediction.diagnosis = analyzed->diagnosis.available
                                   ? analyzed->diagnosis.type
                                   : analysis::IncidentType::unknown;
        prediction.confidence = analyzed->diagnosis.available
                                    ? analyzed->diagnosis.calibrated_confidence
                                    : 0.0;
        if (calibration && analyzed->diagnosis.available) {
            prediction.confidence = evaluation::apply_confidence_calibration(
                calibration->model, prediction.confidence);
            if (!calibration->assertion.assertions_enabled ||
                prediction.confidence < calibration->assertion.threshold) {
                prediction.diagnosis = analysis::IncidentType::unknown;
            }
        }
        prediction.context = analyzed->workload_context.primary;
        prediction.automatic_capture =
            incident.snapshot->header().window.automatic_trigger_count != 0U;
        prediction.recurrence_cluster = cluster_by_id[incident.summary.id];
        const auto pressure = std::max_element(
            analyzed->resources.begin(), analyzed->resources.end(),
            [](const auto& left, const auto& right) {
                if (left.score != right.score) return left.score < right.score;
                return left.resource > right.resource;
            });
        if (pressure != analyzed->resources.end() && pressure->score > 0.0) {
            prediction.observed_pressure = pressure->resource;
            prediction.practical_pressure_score = pressure->score;
            prediction.raw_statistical_score = pressure->statistical_score;
        }
        for (const auto& contributor : analyzed->contributors) {
            if (const auto ordinal = ordinals.find(contributor.identity);
                ordinal != ordinals.end()) {
                prediction.contributor_ordinals.push_back(ordinal->second);
                if (prediction.contributor_ordinals.size() == 20U) break;
            }
        }
        predictions.push_back(std::move(prediction));
    }
    auto report = evaluation::evaluate_diagnostics(*corpus, predictions, split);
    if (!report) {
        std::cerr << "Evaluation failed: " << report.error().message << '\n';
        return 1;
    }
    if (report->predictions_missing != 0U) {
        std::cerr << "Archive is missing " << report->predictions_missing
                  << " truth-linked incidents; refusing a partial report.\n";
        return 1;
    }
    std::optional<evaluation::ConfidenceCalibrationArtifact> fitted_calibration;
    if (split == evaluation::CorpusSplit::calibration) {
        std::map<std::string, const evaluation::IncidentTruth*> truth;
        for (const auto& row : corpus->incidents) {
            if (row.split == split) truth[row.incident_key] = &row;
        }
        for (const auto& prediction : predictions) {
            const auto found = truth.find(prediction.incident_key);
            if (found == truth.end() || found->second->disagreement ||
                (found->second->certainty != evaluation::TruthCertainty::confirmed &&
                 found->second->certainty != evaluation::TruthCertainty::probable) ||
                prediction.diagnosis == analysis::IncidentType::unknown) {
                continue;
            }
            calibration_samples.push_back({
                prediction.confidence,
                prediction.diagnosis == found->second->expected_diagnosis});
        }
        auto model = evaluation::fit_isotonic_confidence_calibration(
            std::move(calibration_samples));
        if (!model) {
            std::cerr << "Calibration split has fewer than ten eligible emitted diagnoses.\n";
            return 1;
        }
        std::vector<evaluation::CalibrationSample> calibrated_samples;
        calibrated_samples.reserve(model->source_samples);
        for (const auto& prediction : predictions) {
            const auto found = truth.find(prediction.incident_key);
            if (found == truth.end() || found->second->disagreement ||
                (found->second->certainty != evaluation::TruthCertainty::confirmed &&
                 found->second->certainty != evaluation::TruthCertainty::probable) ||
                prediction.diagnosis == analysis::IncidentType::unknown) continue;
            calibrated_samples.push_back({
                evaluation::apply_confidence_calibration(*model, prediction.confidence),
                found->second->expected_diagnosis != analysis::IncidentType::unknown &&
                    prediction.diagnosis == found->second->expected_diagnosis});
        }
        const auto assertion = evaluation::select_assertion_threshold(
            std::move(calibrated_samples), 0.80);
        fitted_calibration = evaluation::ConfidenceCalibrationArtifact{
            corpus->manifest.annotation_fingerprint, configuration,
            std::move(*model), assertion};
    }

    if (!write_report(output_transaction->staging_directory, *corpus, *report,
                      predictions, calibration,
                      calibration_artifact_fingerprint.value_or(0U))) {
        std::cerr << "Cannot write evaluation output; partial staging was retained.\n";
        return 1;
    }
    if (fitted_calibration &&
        !evaluation::write_confidence_calibration_artifact(
            output_transaction->staging_directory / "calibration.tsv",
            *fitted_calibration)) {
        std::cerr << "Cannot write calibration artifact.\n";
        return 1;
    }

    const evaluation::DiagnosticEvaluationArtifactMetadata expected_metadata{
        calibration.has_value(),
        calibration_artifact_fingerprint.value_or(0U),
        calibration && calibration->assertion.assertions_enabled,
        calibration ? calibration->assertion.threshold : 0.0};
    auto verified = evaluation::verify_diagnostic_evaluation_artifact(
        output_transaction->staging_directory, *corpus);
    if (!verified || verified->split != split ||
        verified->metadata != expected_metadata || verified->report != *report ||
        verified->prediction_rows != predictions.size()) {
        std::cerr << "Evaluation output failed independent direct-V1 verification; "
                     "partial staging was retained.\n";
        return 1;
    }

    std::optional<std::uint64_t> report_artifact_fingerprint;
    if (split == evaluation::CorpusSplit::held_out) {
        const std::array report_files{
            output_transaction->staging_directory / "evaluation.json",
            output_transaction->staging_directory / "predictions.tsv"};
        auto fingerprint = evaluation::evaluation_artifact_fingerprint(report_files);
        if (!fingerprint) {
            std::cerr << "Held-out report cannot be fingerprinted: "
                      << fingerprint.error().message << '\n';
            return 1;
        }
        report_artifact_fingerprint = *fingerprint;
    }

    if (split == evaluation::CorpusSplit::calibration) {
        constexpr std::array required{
            std::string_view{"evaluation.json"},
            std::string_view{"predictions.tsv"},
            std::string_view{"calibration.tsv"}};
        if (auto published = evaluation::publish_evaluation_output(
                *output_transaction, required); !published) {
            std::cerr << "Cannot publish calibration output: "
                      << published.error().message << '\n';
            return 1;
        }
    } else {
        constexpr std::array required{
            std::string_view{"evaluation.json"},
            std::string_view{"predictions.tsv"}};
        if (auto published = evaluation::publish_evaluation_output(
                *output_transaction, required); !published) {
            std::cerr << "Cannot publish evaluation output: "
                      << published.error().message << '\n';
            return 1;
        }
    }

    auto published_verification = evaluation::verify_diagnostic_evaluation_artifact(
        output_path, *corpus);
    if (!published_verification || published_verification->split != split ||
        published_verification->metadata != expected_metadata ||
        published_verification->report != *report ||
        published_verification->prediction_rows != predictions.size()) {
        std::cerr << "Published evaluation output failed independent direct-V1 "
                     "verification; the held-out attempt remains running.\n";
        return 1;
    }
    if (fitted_calibration) {
        auto published_calibration =
            evaluation::load_confidence_calibration_artifact(
                std::filesystem::path{output_path} / "calibration.tsv");
        if (!published_calibration ||
            *published_calibration != *fitted_calibration) {
            std::cerr << "Published calibration output failed direct-V1 "
                         "verification.\n";
            return 1;
        }
    }
    if (split == evaluation::CorpusSplit::held_out) {
        const std::array published_report_files{
            std::filesystem::path{output_path} / "evaluation.json",
            std::filesystem::path{output_path} / "predictions.tsv"};
        auto published_fingerprint = evaluation::evaluation_artifact_fingerprint(
            published_report_files);
        if (!published_fingerprint ||
            *published_fingerprint != *report_artifact_fingerprint) {
            std::cerr << "Published held-out fingerprint does not match staging; "
                         "the attempt remains running.\n";
            return 1;
        }
    }

    const auto qualification = evaluation::qualify_v0151(*report);
    if (split == evaluation::CorpusSplit::held_out) {
        if (auto completed = evaluation::complete_held_out_evaluation_attempt(
                *held_out_attempt, qualification.passed,
                *report_artifact_fingerprint); !completed) {
            std::cerr << "Cannot complete the held-out attempt record: "
                      << completed.error().message << '\n';
            return 1;
        }
    }
    std::cout << "Evaluated " << report->predictions_matched << '/' << report->truth_rows
              << " " << evaluation::to_string(split) << " incidents; supported_recall="
              << report->supported_diagnosis_recall.rate << " top1="
              << report->top1_contributor_accuracy.rate << " top3="
              << report->top3_contributor_accuracy.rate << " unknown="
              << report->unknown_rate.rate << " ECE="
              << report->expected_calibration_error
              << ".\n";
    if (split == evaluation::CorpusSplit::held_out && !qualification.passed) {
        std::cerr << "V0.15.1 held-out qualification failed: requires >=80% precision, "
                     ">=60% supported recall, >=90% Unknown abstention, and >=70% top-3.\n";
        return 3;
    }
    return 0;
}

int inspect_incident(const std::filesystem::path& archive_path,
                     const std::string_view incident_key) {
    if (!std::filesystem::is_regular_file(archive_path) || incident_key.size() != 32U) {
        std::cerr << "Archive or incident key is invalid.\n";
        return 1;
    }
    storage::SqliteIncidentArchive archive{
        storage::ArchiveConfiguration{
            .path = archive_path,
            .open_mode = storage::ArchiveOpenMode::read_only}};
    if (auto opened = archive.open(); !opened) {
        std::cerr << "Archive open failed: " << opened.error().message << '\n';
        return 1;
    }
    storage::StoredIncidentSummary selected{};
    bool found{};
    for (const auto& summary : list_all(archive)) {
        if (export_key_text(summary.export_key) == incident_key) {
            selected = summary;
            found = true;
            break;
        }
    }
    if (!found) {
        std::cerr << "Incident key was not found.\n";
        return 1;
    }
    auto snapshot = archive.load(selected.id);
    if (!snapshot) {
        std::cerr << "Incident load failed: " << snapshot.error().message << '\n';
        return 1;
    }
    analysis::IntelligentIncidentAnalyzer analyzer;
    auto analyzed = analyzer.analyze(**snapshot);
    if (!analyzed) {
        std::cerr << "Analysis failed: " << analyzed.error().message << '\n';
        return 1;
    }
    const auto ordinals = process_ordinals(**snapshot);
    std::map<core::IncidentProcessIdentity, const core::IncidentProcessInfo*> metadata;
    for (const auto& process : (*snapshot)->process_metadata()) {
        metadata.emplace(process.identity, &process);
    }
    std::cout << "incident_id=" << selected.id
              << " diagnosis="
              << evaluation::to_string(analyzed->diagnosis.available
                     ? analyzed->diagnosis.type : analysis::IncidentType::unknown)
              << " confidence=" << analyzed->diagnosis.calibrated_confidence
              << " context=" << evaluation::to_string(analyzed->workload_context.primary)
              << " automatic_triggers="
              << (*snapshot)->header().window.automatic_trigger_count << '\n'
              << "resources\n";
    for (const auto& resource : analyzed->resources) {
        std::cout << resource_name(resource.resource) << '\t' << resource.score
                  << '\t' << resource.uncontextualized_score << '\t'
                  << resource.context_multiplier << '\n';
    }
    std::cout
              << "ordinal\tpid\tname\n";
    for (const auto& [identity, ordinal] : ordinals) {
        const auto info = metadata.find(identity);
        std::string name{"<unavailable>"};
        if (info != metadata.end() &&
            info->second->name.status == core::RecordedValueStatus::available) {
            name = info->second->name.value;
        }
        std::cout << ordinal << '\t' << identity.pid << '\t' << name << '\n';
    }
    std::cout << "contributors\n"
              << "rank\tordinal\tname\tscore\ttiming\tmetric\tactivity\t"
                 "resource_match\tcoverage\tconfidence\n";
    for (std::size_t index = 0U; index < analyzed->contributors.size(); ++index) {
        const auto& contributor = analyzed->contributors[index];
        const auto ordinal = ordinals.find(contributor.identity);
        std::cout << index << '\t'
                  << (ordinal == ordinals.end() ? std::string{"?"}
                                                : std::to_string(ordinal->second))
                  << '\t' << contributor.name << '\t' << contributor.score << '\t'
                  << (contributor.temporal_relationship ==
                              analysis::ContributorTemporalRelationship::preceding_activity
                          ? "preceding" : "reaction")
                  << '\t' << static_cast<unsigned>(contributor.strongest_metric)
                  << '\t' << contributor.anomaly_magnitude
                  << '\t' << contributor.resource_match_score
                  << '\t' << contributor.evidence_coverage
                  << '\t' << static_cast<unsigned>(contributor.confidence) << '\n';
    }
    return 0;
}

int inspect_incident_truth(const std::filesystem::path& archive_path,
                           const std::string_view incident_key) {
    if (!std::filesystem::is_regular_file(archive_path) || incident_key.size() != 32U) {
        std::cerr << "Archive or incident key is invalid.\n";
        return 1;
    }
    storage::SqliteIncidentArchive archive{
        storage::ArchiveConfiguration{
            .path = archive_path,
            .open_mode = storage::ArchiveOpenMode::read_only}};
    if (auto opened = archive.open(); !opened) {
        std::cerr << "Archive open failed: " << opened.error().message << '\n';
        return 1;
    }
    storage::StoredIncidentSummary selected{};
    bool found{};
    for (const auto& summary : list_all(archive)) {
        if (export_key_text(summary.export_key) == incident_key) {
            selected = summary;
            found = true;
            break;
        }
    }
    if (!found) {
        std::cerr << "Incident key was not found.\n";
        return 1;
    }
    auto snapshot = archive.load(selected.id);
    if (!snapshot) {
        std::cerr << "Incident load failed: " << snapshot.error().message << '\n';
        return 1;
    }
    const auto ordinals = process_ordinals(**snapshot);
    std::map<core::IncidentProcessIdentity, const core::IncidentProcessInfo*> metadata;
    for (const auto& process : (*snapshot)->process_metadata()) {
        metadata.emplace(process.identity, &process);
    }
    std::cout << "incident_id=" << selected.id
              << " event_monotonic_nanoseconds=" << selected.event_monotonic_nanoseconds
              << " actual_start_nanoseconds=" << selected.actual_start_nanoseconds
              << " actual_end_nanoseconds=" << selected.actual_end_nanoseconds
              << " system_samples=" << selected.system_sample_count
              << " process_samples=" << selected.process_sample_count
              << " system_events=" << (*snapshot)->system_events().size() << '\n'
              << "ordinal\tpid\tname\n";
    for (const auto& [identity, ordinal] : ordinals) {
        const auto info = metadata.find(identity);
        std::string name{"<unavailable>"};
        if (info != metadata.end() &&
            info->second->name.status == core::RecordedValueStatus::available) {
            name = info->second->name.value;
        }
        std::cout << ordinal << '\t' << identity.pid << '\t' << name << '\n';
    }
    return 0;
}

int export_incident_truth(const std::filesystem::path& archive_path,
                          const std::string_view incident_key,
                          const std::filesystem::path& output_path,
                          const std::string_view privacy_mode) {
    if (!std::filesystem::is_regular_file(archive_path) || incident_key.size() != 32U) {
        std::cerr << "Archive or incident key is invalid.\n";
        return 1;
    }
    evaluation::TruthReviewOptions options{};
    if (privacy_mode == "ordinal-only") {
        options.include_local_process_identities = false;
    } else if (privacy_mode == "include-local-identities") {
        options.include_local_process_identities = true;
    } else {
        std::cerr << "Truth review privacy mode must be ordinal-only or "
                     "include-local-identities.\n";
        return 2;
    }
    storage::SqliteIncidentArchive archive{
        storage::ArchiveConfiguration{
            .path = archive_path,
            .open_mode = storage::ArchiveOpenMode::read_only}};
    if (auto opened = archive.open(); !opened) {
        std::cerr << "Archive open failed: " << opened.error().message << '\n';
        return 1;
    }
    storage::StoredIncidentSummary selected{};
    bool found{};
    try {
        for (const auto& summary : list_all(archive)) {
            if (export_key_text(summary.export_key) == incident_key) {
                selected = summary;
                found = true;
                break;
            }
        }
    } catch (const std::exception& exception) {
        std::cerr << "Archive listing failed: " << exception.what() << '\n';
        return 1;
    }
    if (!found) {
        std::cerr << "Incident key was not found.\n";
        return 1;
    }
    auto snapshot = archive.load(selected.id);
    if (!snapshot) {
        std::cerr << "Incident load failed: " << snapshot.error().message << '\n';
        return 1;
    }
    auto exported = evaluation::export_truth_review(
        **snapshot, incident_key, selected.created_utc_milliseconds, output_path, options);
    if (!exported) {
        std::cerr << "Truth review export failed: " << exported.error().message << '\n';
        return 1;
    }
    std::cout << "Published prediction-free truth review with "
              << exported->system_samples << " system samples, "
              << exported->process_samples << " process samples, "
              << exported->processes << " process ordinals, and "
              << exported->system_events << " system events.\n";
    if (exported->local_process_identities) {
        std::cout << "Local process identities are included by explicit request; "
                     "do not copy this artifact into the corpus.\n";
    }
    return 0;
}

int list_incident_truth(const std::filesystem::path& archive_path) {
    if (!std::filesystem::is_regular_file(archive_path)) {
        std::cerr << "Archive is not a regular file.\n";
        return 1;
    }
    storage::SqliteIncidentArchive archive{
        storage::ArchiveConfiguration{
            .path = archive_path,
            .open_mode = storage::ArchiveOpenMode::read_only}};
    if (auto opened = archive.open(); !opened) {
        std::cerr << "Archive open failed: " << opened.error().message << '\n';
        return 1;
    }
    std::cout << "incident_key\tcreated_utc_ms\tsystem_samples\tprocess_samples\n";
    try {
        for (const auto& summary : list_all(archive)) {
            std::cout << export_key_text(summary.export_key) << '\t'
                      << summary.created_utc_milliseconds << '\t'
                      << summary.system_sample_count << '\t'
                      << summary.process_sample_count << '\n';
        }
    } catch (const std::exception& exception) {
        std::cerr << "Archive listing failed: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}

int merge_session_packet(const std::filesystem::path& base_corpus_path,
                         const std::filesystem::path& packet_path,
                         const std::filesystem::path& archive_path,
                         const std::filesystem::path& output_path) {
    auto packet = evaluation::load_dogfood_corpus(packet_path);
    if (!packet) {
        std::cerr << "Session packet invalid: " << packet.error().message << '\n';
        return 1;
    }
    if (packet->sessions.size() != 1U || packet->hardware_profiles.size() != 1U) {
        std::cerr << "Session packet must contain exactly one session and hardware profile.\n";
        return 1;
    }
    evaluation::DogfoodSessionArchiveEvidence evidence{};
    if (packet->incidents.empty()) {
        if (archive_path != "none") {
            std::cerr << "A zero-incident session packet requires archive argument 'none'.\n";
            return 1;
        }
    } else {
        if (archive_path == "none" || !std::filesystem::is_regular_file(archive_path)) {
            std::cerr << "A session packet with incidents requires a regular schema-v1 archive.\n";
            return 1;
        }
        storage::SqliteIncidentArchive archive{
            storage::ArchiveConfiguration{
                .path = archive_path,
                .open_mode = storage::ArchiveOpenMode::read_only}};
        if (auto opened = archive.open(); !opened) {
            std::cerr << "Packet archive open failed: " << opened.error().message << '\n';
            return 1;
        }
        std::map<std::string, storage::StoredIncidentSummary, std::less<>> summaries;
        try {
            for (auto& summary : list_all(archive)) {
                summaries.emplace(export_key_text(summary.export_key), std::move(summary));
            }
        } catch (const std::exception& exception) {
            std::cerr << "Packet archive listing failed: " << exception.what() << '\n';
            return 1;
        }
        evidence.incident_keys.reserve(packet->incidents.size());
        for (const auto& incident : packet->incidents) {
            const auto summary = summaries.find(incident.incident_key);
            if (summary == summaries.end()) {
                std::cerr << "Packet incident is absent from the supplied archive: "
                          << incident.incident_key << '\n';
                return 1;
            }
            auto snapshot = archive.load(summary->second.id);
            if (!snapshot) {
                std::cerr << "Packet incident load failed: "
                          << snapshot.error().message << '\n';
                return 1;
            }
            evidence.incident_keys.push_back(incident.incident_key);
            if ((*snapshot)->header().window.automatic_trigger_count != 0U) {
                ++evidence.automatic_captures;
            }
        }
    }
    if (auto merged = evaluation::merge_dogfood_session_packet(
            base_corpus_path, packet_path, evidence, output_path); !merged) {
        std::cerr << "Session packet merge failed: " << merged.error().message << '\n';
        return 1;
    }
    std::cout << "Merged session " << packet->sessions.front().session_id
              << " into a new collecting corpus at " << output_path.string() << ".\n";
    return 0;
}

void usage() {
    std::cerr <<
        "Usage:\n"
        "  blackbox_dogfood_tool fingerprint\n"
        "  blackbox_dogfood_tool init <new-corpus-directory> <corpus-id>\n"
        "  blackbox_dogfood_tool init-session <base-corpus-directory> "
        "<new-session-packet-directory>\n"
        "  blackbox_dogfood_tool validate <corpus-directory>\n"
        "  blackbox_dogfood_tool readiness <corpus-directory>\n"
        "  blackbox_dogfood_tool campaign-status <corpus-directory> "
        "<new-output-directory>\n"
        "  blackbox_dogfood_tool validate-ballot <completed-ballot.tsv> "
        "<expected-incident-key> <session-operator-id>\n"
        "  blackbox_dogfood_tool compare-ballots <completed-ballot-a.tsv> "
        "<completed-ballot-b.tsv> <expected-incident-key> <session-operator-id>\n"
        "  blackbox_dogfood_tool heldout-status <corpus-directory>\n"
        "  blackbox_dogfood_tool verify-evaluation <frozen-corpus-directory> "
        "<evaluation-output-directory> <calibration.tsv|none>\n"
        "  blackbox_dogfood_tool freeze <corpus-directory> <excluded-incidents.tsv>\n"
        "  blackbox_dogfood_tool merge-session <base-corpus-directory> "
        "<session-packet-directory> <archive.sqlite3|none> <new-corpus-directory>\n"
        "  blackbox_dogfood_tool list-truth <archive.sqlite3>\n"
        "  blackbox_dogfood_tool inspect-truth <archive.sqlite3> <incident-key>\n"
        "  blackbox_dogfood_tool export-truth <archive.sqlite3> <incident-key> "
        "<new-review-directory> <ordinal-only|include-local-identities>\n"
        "  blackbox_dogfood_tool inspect <archive.sqlite3> <incident-key>\n"
        "  blackbox_dogfood_tool evaluate <archive-map.tsv> <corpus-directory> "
        "<development|calibration|held_out> <calibration.tsv|none> <new-output-directory>\n";
}

} // namespace

int main(const int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view{argv[1]} == "fingerprint") {
            const auto pipeline = current_pipeline_identity();
            std::cout << "pipeline_version=" << pipeline.version << '\n'
                      << "configuration_fingerprint="
                      << pipeline.configuration_fingerprint << '\n';
            return 0;
        }
        if (argc == 4 && std::string_view{argv[1]} == "init") {
            const auto pipeline = current_pipeline_identity();
            auto initialized = evaluation::initialize_dogfood_corpus(
                std::filesystem::path{argv[2]}, argv[3], pipeline.version,
                pipeline.configuration_fingerprint);
            if (!initialized) {
                std::cerr << "Initialization failed: " << initialized.error().message << '\n';
                return 1;
            }
            std::cout << "Initialized dogfood protocol v" << evaluation::dogfood_protocol_version
                      << " corpus.\n";
            return 0;
        }
        if (argc == 4 && std::string_view{argv[1]} == "init-session") {
            const auto pipeline = current_pipeline_identity();
            return initialize_session_packet(
                argv[2], argv[3], pipeline.version,
                pipeline.configuration_fingerprint);
        }
        if (argc == 3 && std::string_view{argv[1]} == "validate") {
            return report_corpus(argv[2], false);
        }
        if (argc == 3 && std::string_view{argv[1]} == "readiness") {
            return report_corpus(argv[2], true);
        }
        if (argc == 4 && std::string_view{argv[1]} == "campaign-status") {
            return export_campaign_readiness(argv[2], argv[3]);
        }
        if (argc == 5 && std::string_view{argv[1]} == "validate-ballot") {
            return validate_annotation_ballot(argv[2], argv[3], argv[4]);
        }
        if (argc == 6 && std::string_view{argv[1]} == "compare-ballots") {
            return compare_annotation_ballots(argv[2], argv[3], argv[4], argv[5]);
        }
        if (argc == 3 && std::string_view{argv[1]} == "heldout-status") {
            return report_held_out_status(argv[2]);
        }
        if (argc == 5 && std::string_view{argv[1]} == "verify-evaluation") {
            return verify_evaluation_output(argv[2], argv[3], argv[4]);
        }
        if (argc == 4 && std::string_view{argv[1]} == "freeze") {
            auto frozen = evaluation::freeze_dogfood_corpus(argv[2], argv[3]);
            if (!frozen) {
                std::cerr << "Freeze failed: " << frozen.error().message << '\n';
                return 1;
            }
            std::cout << "Frozen annotation fingerprint " << *frozen << ".\n";
            return 0;
        }
        if (argc == 4 && std::string_view{argv[1]} == "inspect") {
            return inspect_incident(argv[2], argv[3]);
        }
        if (argc == 4 && std::string_view{argv[1]} == "inspect-truth") {
            return inspect_incident_truth(argv[2], argv[3]);
        }
        if (argc == 3 && std::string_view{argv[1]} == "list-truth") {
            return list_incident_truth(argv[2]);
        }
        if (argc == 6 && std::string_view{argv[1]} == "export-truth") {
            return export_incident_truth(argv[2], argv[3], argv[4], argv[5]);
        }
        if (argc == 6 && std::string_view{argv[1]} == "merge-session") {
            return merge_session_packet(argv[2], argv[3], argv[4], argv[5]);
        }
        if (argc == 7 && std::string_view{argv[1]} == "evaluate") {
            const auto split = parse_split(argv[4]);
            if (!split) {
                std::cerr << "Unknown corpus split.\n";
                return 2;
            }
            return evaluate(argv[2], argv[3], *split, argv[5], argv[6]);
        }
        usage();
        return 2;
    } catch (const std::exception& exception) {
        std::cerr << "Dogfood tool failed: " << exception.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dogfood tool failed with an unknown error.\n";
        return 1;
    }
}
