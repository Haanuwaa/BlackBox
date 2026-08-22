#include "evaluation/diagnostic_evaluation_artifact.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace analysis = blackbox::analysis;
namespace evaluation = blackbox::evaluation;

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
            ("blackbox-diagnostic-artifact-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path{};
};

void write_file(const std::filesystem::path& path, const std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

[[nodiscard]] evaluation::IncidentTruth truth(
    const char key, const analysis::IncidentType diagnosis,
    const std::optional<std::size_t> contributor,
    const std::string& family = {}) {
    return {std::string(31U, '0') + key, "held",
            evaluation::CorpusSplit::held_out,
            evaluation::SymptomClass::cpu_starvation,
            evaluation::TruthCertainty::confirmed, true, diagnosis, contributor,
            analysis::WorkloadContextKind::development, family, true,
            evaluation::UsefulnessRating::useful, 2U, false};
}

[[nodiscard]] evaluation::DogfoodCorpus corpus() {
    evaluation::DogfoodCorpus value{};
    value.manifest = {"artifact-v1", true, 12U, 456U, 789U};
    value.hardware_profiles.push_back(
        {"host-a", "windows", "win11", "x64", 8U, "16-31", "unknown",
         "balanced"});
    value.sessions.push_back(
        {"held", "host-a", "operator-a", evaluation::CorpusSplit::held_out,
         evaluation::DogfoodSessionKind::natural,
         evaluation::SymptomClass::cpu_starvation, 600.0, 4U, 0U, true});
    value.sessions.push_back(
        {"quiet", "host-a", "operator-a", evaluation::CorpusSplit::held_out,
         evaluation::DogfoodSessionKind::quiet,
         evaluation::SymptomClass::quiet, 3'600.0, 0U, 1U, true});
    value.incidents = {
        truth('1', analysis::IncidentType::cpu_pressure, 2U, "family-a"),
        truth('2', analysis::IncidentType::cpu_pressure, 3U, "family-a"),
        truth('3', analysis::IncidentType::storage_pressure, std::nullopt),
        truth('4', analysis::IncidentType::unknown, std::nullopt),
    };
    return value;
}

[[nodiscard]] std::vector<evaluation::DiagnosticPrediction> predictions() {
    return {
        {std::string(31U, '0') + '1', analysis::IncidentType::cpu_pressure, 0.9,
         {2U, 7U}, analysis::WorkloadContextKind::development, true, "cluster-a",
         analysis::ResourceKind::cpu, 0.8, 0.9},
        {std::string(31U, '0') + '2', analysis::IncidentType::cpu_pressure, 0.8,
         {9U, 3U}, analysis::WorkloadContextKind::development, true, "cluster-a",
         analysis::ResourceKind::cpu, 0.7, 0.8},
        {std::string(31U, '0') + '3', analysis::IncidentType::network_pressure, 0.7,
         {}, analysis::WorkloadContextKind::desktop, false, {},
         analysis::ResourceKind::network, 0.6, 0.7},
        {std::string(31U, '0') + '4', analysis::IncidentType::unknown, 0.0,
         {}, analysis::WorkloadContextKind::development, true, {}, std::nullopt,
         0.0, 0.0},
    };
}

void replace_once(std::string& text, const std::string_view needle,
                  const std::string_view replacement) {
    const auto offset = text.find(needle);
    REQUIRE(offset != std::string::npos);
    text.replace(offset, needle.size(), replacement);
}

} // namespace

TEST_CASE("diagnostic evaluation artifacts are canonical recomputable and tamper evident",
          "[evaluation][artifact][direct-v1]") {
    TemporaryDirectory temporary;
    const auto input = corpus();
    const auto prediction_rows = predictions();
    const auto report = evaluation::evaluate_diagnostics(input, prediction_rows);
    REQUIRE(report.has_value());
    const evaluation::DiagnosticEvaluationArtifactMetadata metadata{
        true, 123'456U, true, 0.75};
    const auto json = evaluation::serialize_diagnostic_evaluation_json(
        input, *report, metadata);
    const auto tsv = evaluation::serialize_diagnostic_predictions_tsv(prediction_rows);
    REQUIRE(json.has_value());
    REQUIRE(tsv.has_value());
    CHECK(json->find("\"application_crash\":0") != std::string::npos);
    write_file(temporary.path / "evaluation.json", *json);
    write_file(temporary.path / "predictions.tsv", *tsv);

    const auto verified = evaluation::verify_diagnostic_evaluation_artifact(
        temporary.path, input);
    REQUIRE(verified.has_value());
    CHECK(verified->split == evaluation::CorpusSplit::held_out);
    CHECK(verified->metadata == metadata);
    CHECK(verified->report == *report);
    CHECK(verified->prediction_rows == 4U);
    CHECK_FALSE(verified->qualification_passed);

    auto tampered_json = *json;
    replace_once(tampered_json,
                 "\"supported_diagnosis_recall\":{\"eligible\":3,"
                 "\"successful\":2",
                 "\"supported_diagnosis_recall\":{\"eligible\":3,"
                 "\"successful\":3");
    write_file(temporary.path / "evaluation.json", tampered_json);
    auto rejected = evaluation::verify_diagnostic_evaluation_artifact(
        temporary.path, input);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code ==
          evaluation::DiagnosticEvaluationArtifactErrorCode::content_mismatch);

    auto wrong_calibration = *json;
    replace_once(wrong_calibration,
                 "\"calibration_artifact_fingerprint\":123456",
                 "\"calibration_artifact_fingerprint\":123457");
    write_file(temporary.path / "evaluation.json", wrong_calibration);
    rejected = evaluation::verify_diagnostic_evaluation_artifact(
        temporary.path, input);
    REQUIRE(rejected.has_value());
    CHECK(rejected->metadata.calibration_artifact_fingerprint == 123'457U);

    write_file(temporary.path / "evaluation.json", *json);
    auto sparse_rows = prediction_rows;
    sparse_rows.pop_back();
    const auto sparse_tsv = evaluation::serialize_diagnostic_predictions_tsv(sparse_rows);
    REQUIRE(sparse_tsv.has_value());
    write_file(temporary.path / "predictions.tsv", *sparse_tsv);
    rejected = evaluation::verify_diagnostic_evaluation_artifact(
        temporary.path, input);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code ==
          evaluation::DiagnosticEvaluationArtifactErrorCode::content_mismatch);

    auto duplicate_ordinals = prediction_rows;
    duplicate_ordinals.front().contributor_ordinals = {2U, 2U};
    const auto duplicate_tsv = evaluation::serialize_diagnostic_predictions_tsv(
        duplicate_ordinals);
    REQUIRE_FALSE(duplicate_tsv.has_value());
    CHECK(duplicate_tsv.error().code ==
          evaluation::DiagnosticEvaluationArtifactErrorCode::invalid_format);

    write_file(temporary.path / "predictions.tsv", *tsv);
    auto wrong_corpus = input;
    ++wrong_corpus.manifest.annotation_fingerprint;
    rejected = evaluation::verify_diagnostic_evaluation_artifact(
        temporary.path, wrong_corpus);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code ==
          evaluation::DiagnosticEvaluationArtifactErrorCode::invalid_format);

    auto excess_rows = *tsv;
    const auto header_break = excess_rows.find('\n');
    REQUIRE(header_break != std::string::npos);
    const auto first_row_begin = header_break + 1U;
    const auto first_row_break = excess_rows.find('\n', first_row_begin);
    REQUIRE(first_row_break != std::string::npos);
    const auto first_row_end = first_row_break + 1U;
    excess_rows.append(excess_rows.substr(first_row_begin,
                                          first_row_end - first_row_begin));
    write_file(temporary.path / "predictions.tsv", excess_rows);
    rejected = evaluation::verify_diagnostic_evaluation_artifact(
        temporary.path, input);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code ==
          evaluation::DiagnosticEvaluationArtifactErrorCode::limit_exceeded);

    const auto tsv_header_break = tsv->find('\n');
    REQUIRE(tsv_header_break != std::string::npos);
    const auto header_end = tsv_header_break + 1U;
    const auto oversized_row = tsv->substr(0U, header_end) +
        std::string(evaluation::maximum_diagnostic_prediction_row_bytes + 1U, 'x') +
        '\n';
    write_file(temporary.path / "predictions.tsv", oversized_row);
    rejected = evaluation::verify_diagnostic_evaluation_artifact(
        temporary.path, input);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code ==
          evaluation::DiagnosticEvaluationArtifactErrorCode::invalid_format);
}
