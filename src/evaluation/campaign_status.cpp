#include "evaluation/campaign_status.hpp"

#include <fstream>
#include <iomanip>
#include <string_view>
#include <utility>

namespace blackbox::evaluation {
namespace {

[[nodiscard]] CampaignStatusError status_error(const CampaignStatusErrorCode code,
                                                std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] std::string tsv_text(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        result.push_back(character == '\t' || character == '\r' || character == '\n'
                             ? ' '
                             : character);
    }
    return result;
}

[[nodiscard]] bool valid_manifest_text(const std::string_view value) noexcept {
    return !value.empty() && value.find_first_of("\t\r\n") == std::string_view::npos;
}

[[nodiscard]] std::string html_text(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&#39;"; break;
        default: result.push_back(character); break;
        }
    }
    return result;
}

[[nodiscard]] bool stream_ok(std::ofstream& output) {
    output.flush();
    return static_cast<bool>(output);
}

class StagingCleanup final {
public:
    explicit StagingCleanup(std::filesystem::path path) : path_(std::move(path)) {}
    ~StagingCleanup() {
        if (!published_) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }
    void published() noexcept { published_ = true; }

private:
    std::filesystem::path path_{};
    bool published_{};
};

} // namespace

std::expected<CampaignStatusStatistics, CampaignStatusError> export_campaign_status(
    const DogfoodCorpus& corpus, const std::int64_t created_utc_milliseconds,
    const std::filesystem::path& destination) noexcept {
    try {
        if (created_utc_milliseconds < 0 || destination.empty() ||
            destination.filename().empty() || destination.filename() == "." ||
            destination.filename() == "..") {
            return std::unexpected{status_error(CampaignStatusErrorCode::invalid_input,
                                                "invalid campaign-status destination or time")};
        }
        if (!valid_manifest_text(corpus.manifest.corpus_id) ||
            corpus.manifest.pipeline_version == 0U ||
            corpus.manifest.configuration_fingerprint == 0U) {
            return std::unexpected{status_error(CampaignStatusErrorCode::corpus_invalid,
                                                "invalid campaign-status corpus provenance")};
        }
        auto report = assess_dogfood_qualification(corpus);
        if (!report) {
            return std::unexpected{status_error(CampaignStatusErrorCode::corpus_invalid,
                                                report.error().message)};
        }

        auto staging = destination;
        staging += ".partial";
        std::error_code issue;
        if (std::filesystem::exists(destination, issue) || issue ||
            std::filesystem::exists(staging, issue) || issue) {
            return std::unexpected{status_error(CampaignStatusErrorCode::destination_exists,
                                                "campaign-status destination or staging exists")};
        }
        const auto parent = destination.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, issue);
            if (issue) {
                return std::unexpected{status_error(CampaignStatusErrorCode::io,
                                                    "cannot create campaign-status parent")};
            }
        }
        if (!std::filesystem::create_directory(staging, issue) || issue) {
            return std::unexpected{status_error(CampaignStatusErrorCode::io,
                                                "cannot create campaign-status staging")};
        }
        StagingCleanup cleanup{staging};

        {
            std::ofstream output{staging / "manifest.ini", std::ios::binary};
            output << "format=" << campaign_status_format_version << '\n'
                   << "artifact=blackbox-dogfood-campaign-status\n"
                   << "prediction_free=1\n"
                   << "evidence_neutral=1\n"
                   << "created_utc_ms=" << created_utc_milliseconds << '\n'
                   << "corpus_id=" << tsv_text(corpus.manifest.corpus_id) << '\n'
                   << "corpus_state=" << (corpus.manifest.frozen ? "frozen" : "collecting") << '\n'
                   << "qualification_ready=" << (report->ready_to_freeze() ? 1 : 0) << '\n'
                   << "profile_rows=" << report->hardware_qualification.size() << '\n'
                   << "symptom_rows=" << report->calibration_symptom_coverage.size() << '\n'
                   << "unmet_rows=" << report->unmet_requirements.size() << '\n';
            if (!stream_ok(output)) {
                return std::unexpected{status_error(CampaignStatusErrorCode::io,
                                                    "cannot write campaign-status manifest")};
            }
        }
        {
            std::ofstream output{staging / "summary.tsv", std::ios::binary};
            output << std::setprecision(17)
                   << "metric\tvalue\ttarget\tmet\n"
                   << "fully_qualified_hardware_profiles\t"
                   << report->fully_qualified_hardware_profiles << '\t'
                   << minimum_qualification_hardware_profiles << '\t'
                   << (report->fully_qualified_hardware_profiles >=
                       minimum_qualification_hardware_profiles) << '\n'
                   << "natural_sessions\t" << report->natural_sessions << '\t'
                   << minimum_natural_sessions << '\t'
                   << (report->natural_sessions >= minimum_natural_sessions) << '\n'
                   << "quiet_exposure_hours\t" << report->quiet_exposure_seconds / 3'600.0
                   << '\t' << minimum_quiet_exposure_seconds / 3'600.0 << '\t'
                   << (report->quiet_exposure_seconds >= minimum_quiet_exposure_seconds)
                   << '\n'
                   << "calibration_supported_diagnoses\t"
                   << report->calibration_supported_diagnoses << '\t'
                   << minimum_calibration_diagnoses << '\t'
                   << (report->calibration_supported_diagnoses >= minimum_calibration_diagnoses)
                   << '\n'
                   << "held_out_scorable_truth_rows\t" << report->held_out_scorable_truth_rows
                   << '\t' << minimum_held_out_truth_rows << '\t'
                   << (report->held_out_scorable_truth_rows >= minimum_held_out_truth_rows)
                   << '\n'
                   << "insufficient_independent_annotation_rows\t"
                   << report->insufficient_independent_annotation_rows << "\t0\t"
                   << (report->insufficient_independent_annotation_rows == 0U) << '\n';
            if (!stream_ok(output)) {
                return std::unexpected{status_error(CampaignStatusErrorCode::io,
                                                    "cannot write campaign-status summary")};
            }
        }
        {
            std::ofstream output{staging / "profiles.tsv", std::ios::binary};
            output << std::setprecision(17)
                   << "profile_id\tcalibration_natural\theld_out_natural\t"
                      "calibration_quiet_hours\theld_out_quiet_hours\t"
                      "calibration_scorable\theld_out_scorable\tfully_qualified\n";
            for (const auto& profile : report->hardware_qualification) {
                output << tsv_text(profile.profile_id) << '\t' << profile.calibration_natural
                       << '\t' << profile.held_out_natural << '\t'
                       << profile.calibration_quiet_seconds / 3'600.0 << '\t'
                       << profile.held_out_quiet_seconds / 3'600.0 << '\t'
                       << profile.calibration_scorable_truth << '\t'
                       << profile.held_out_scorable_truth << '\t'
                       << profile.fully_qualified() << '\n';
            }
            if (!stream_ok(output)) {
                return std::unexpected{status_error(CampaignStatusErrorCode::io,
                                                    "cannot write campaign-status profiles")};
            }
        }
        {
            std::ofstream output{staging / "symptoms.tsv", std::ios::binary};
            output << "symptom\tcalibration_covered\theld_out_covered\n";
            for (std::size_t index = 0U;
                 index < report->calibration_symptom_coverage.size(); ++index) {
                output << to_string(static_cast<SymptomClass>(index)) << '\t'
                       << report->calibration_symptom_coverage[index] << '\t'
                       << report->held_out_symptom_coverage[index] << '\n';
            }
            if (!stream_ok(output)) {
                return std::unexpected{status_error(CampaignStatusErrorCode::io,
                                                    "cannot write campaign-status symptoms")};
            }
        }
        {
            std::ofstream output{staging / "unmet.tsv", std::ios::binary};
            output << "ordinal\trequirement\n";
            for (std::size_t index = 0U; index < report->unmet_requirements.size(); ++index) {
                output << index + 1U << '\t' << tsv_text(report->unmet_requirements[index]) << '\n';
            }
            if (!stream_ok(output)) {
                return std::unexpected{status_error(CampaignStatusErrorCode::io,
                                                    "cannot write campaign-status requirements")};
            }
        }
        {
            std::ofstream output{staging / "status.html", std::ios::binary};
            output << std::setprecision(12)
                   << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                      "<title>BlackBox campaign readiness</title><style>"
                      ":root{color-scheme:dark}body{font:15px system-ui;background:#0b1020;color:#e8eefc;"
                      "margin:0;padding:32px;max-width:1280px}h1,h2{color:#fff}.notice{padding:14px;"
                      "border:1px solid #52617a;background:#141d31}.grid{display:grid;"
                      "grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:12px;margin:20px 0}"
                      ".card{padding:14px;background:#121a2b;border:1px solid #2f3d57;border-radius:8px}"
                      ".ok{color:#62d49c}.missing{color:#ffae75}table{border-collapse:collapse;width:100%;"
                      "margin-bottom:24px}th,td{border-bottom:1px solid #2f3d57;padding:8px;text-align:left}"
                      "code{color:#b9c9e8}</style></head><body><h1>V0.15.1 campaign readiness</h1>"
                      "<div class=\"notice\"><strong>Prediction-free, evidence-neutral status.</strong> "
                      "This page summarizes declared corpus coverage only. It does not run diagnosis, "
                      "create evidence, or satisfy collection and held-out gates.</div><p>Corpus <code>"
                   << html_text(corpus.manifest.corpus_id) << "</code> is <strong>"
                   << (corpus.manifest.frozen ? "frozen" : "collecting") << "</strong>. Readiness: "
                   << (report->ready_to_freeze()
                           ? "<strong class=\"ok\">ready to freeze</strong>"
                           : "<strong class=\"missing\">not ready</strong>")
                   << ".</p><div class=\"grid\"><div class=\"card\"><b>Qualified profiles</b><br>"
                   << report->fully_qualified_hardware_profiles << " / "
                   << minimum_qualification_hardware_profiles << "</div><div class=\"card\">"
                      "<b>Natural sessions</b><br>" << report->natural_sessions
                   << " / " << minimum_natural_sessions
                   << "</div><div class=\"card\"><b>Quiet exposure</b><br>"
                   << report->quiet_exposure_seconds / 3'600.0
                   << " / " << minimum_quiet_exposure_seconds / 3'600.0
                   << " hours</div><div class=\"card\"><b>Calibration diagnoses</b><br>"
                   << report->calibration_supported_diagnoses
                   << " / " << minimum_calibration_diagnoses
                   << "</div><div class=\"card\"><b>Held-out truth</b><br>"
                   << report->held_out_scorable_truth_rows << " / "
                   << minimum_held_out_truth_rows << "</div></div>"
                      "<h2>Hardware profiles</h2><table><thead><tr><th>Profile</th>"
                      "<th>Calibration natural</th><th>Held-out natural</th>"
                      "<th>Calibration quiet h</th><th>Held-out quiet h</th>"
                      "<th>Calibration truth</th><th>Held-out truth</th><th>Status</th>"
                      "</tr></thead><tbody>";
            for (const auto& profile : report->hardware_qualification) {
                output << "<tr><td>" << html_text(profile.profile_id) << "</td><td>"
                       << (profile.calibration_natural ? "yes" : "missing") << "</td><td>"
                       << (profile.held_out_natural ? "yes" : "missing") << "</td><td>"
                       << profile.calibration_quiet_seconds / 3'600.0 << "</td><td>"
                       << profile.held_out_quiet_seconds / 3'600.0 << "</td><td>"
                       << (profile.calibration_scorable_truth ? "yes" : "missing") << "</td><td>"
                       << (profile.held_out_scorable_truth ? "yes" : "missing") << "</td><td>"
                       << (profile.fully_qualified() ? "qualified" : "incomplete") << "</td></tr>";
            }
            output << "</tbody></table><h2>Symptom coverage</h2><table><thead><tr>"
                      "<th>Symptom</th><th>Calibration</th><th>Held out</th></tr></thead><tbody>";
            for (std::size_t index = 0U;
                 index < report->calibration_symptom_coverage.size(); ++index) {
                output << "<tr><td>" << to_string(static_cast<SymptomClass>(index))
                       << "</td><td>" << (report->calibration_symptom_coverage[index] ? "covered" : "missing")
                       << "</td><td>" << (report->held_out_symptom_coverage[index] ? "covered" : "missing")
                       << "</td></tr>";
            }
            output << "</tbody></table><h2>Unmet requirements</h2>";
            if (report->unmet_requirements.empty()) {
                output << "<p class=\"ok\">None.</p>";
            } else {
                output << "<ol>";
                for (const auto& requirement : report->unmet_requirements) {
                    output << "<li>" << html_text(requirement) << "</li>";
                }
                output << "</ol>";
            }
            output << "</body></html>\n";
            if (!stream_ok(output)) {
                return std::unexpected{status_error(CampaignStatusErrorCode::io,
                                                    "cannot write campaign-status page")};
            }
        }

        std::filesystem::rename(staging, destination, issue);
        if (issue) {
            return std::unexpected{status_error(CampaignStatusErrorCode::io,
                                                "cannot publish campaign-status directory")};
        }
        cleanup.published();
        return CampaignStatusStatistics{report->hardware_qualification.size(),
                                        report->calibration_symptom_coverage.size(),
                                        report->unmet_requirements.size(),
                                        report->ready_to_freeze()};
    } catch (const std::exception& exception) {
        return std::unexpected{status_error(CampaignStatusErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{status_error(CampaignStatusErrorCode::io,
                                            "unknown campaign-status failure")};
    }
}

} // namespace blackbox::evaluation
