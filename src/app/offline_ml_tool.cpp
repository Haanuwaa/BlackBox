#include "analysis/incident_clustering.hpp"
#include "evaluation/diagnostic_evaluation_artifact.hpp"
#include "evaluation/dogfood_corpus.hpp"
#include "evaluation/offline_model_harness.hpp"
#include "storage/incident_archive.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace analysis = blackbox::analysis;
namespace evaluation = blackbox::evaluation;
namespace storage = blackbox::storage;

namespace {

[[nodiscard]] std::string
export_key_text(const storage::IncidentExportKey &key) {
  constexpr std::array digits{'0', '1', '2', '3', '4', '5', '6', '7',
                              '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result(key.bytes.size() * 2U, '0');
  for (std::size_t index = 0U; index < key.bytes.size(); ++index) {
    result[index * 2U] = digits[key.bytes[index] >> 4U];
    result[index * 2U + 1U] = digits[key.bytes[index] & 0x0FU];
  }
  return result;
}

[[nodiscard]] std::vector<storage::StoredIncidentSummary>
list_all(const storage::SqliteIncidentArchive &archive) {
  std::vector<storage::StoredIncidentSummary> result{};
  std::size_t offset{};
  while (result.size() <= evaluation::maximum_offline_feature_rows) {
    const auto page = archive.list_page(storage::IncidentListQuery{
        .offset = offset,
        .limit = storage::maximum_incident_page_size,
        .sort = storage::IncidentListSort::oldest_first});
    if (!page)
      throw std::runtime_error{page.error().message};
    result.insert(result.end(), page->incidents.begin(), page->incidents.end());
    offset += page->incidents.size();
    if (offset >= page->total_matching || page->incidents.empty())
      break;
  }
  return result;
}

int export_features(const std::filesystem::path &archive_path,
                    const std::filesystem::path &destination) {
  if (!std::filesystem::is_regular_file(archive_path)) {
    std::cerr << "Archive is not a regular file.\n";
    return 1;
  }
  storage::SqliteIncidentArchive archive{storage::ArchiveConfiguration{
      .path = archive_path, .open_mode = storage::ArchiveOpenMode::read_only}};
  if (const auto opened = archive.open(); !opened) {
    std::cerr << "Archive open failed: " << opened.error().message << '\n';
    return 1;
  }
  std::vector<evaluation::OfflineFeatureRow> rows{};
  for (const auto &summary : list_all(archive)) {
    const auto snapshot = archive.load(summary.id);
    if (!snapshot) {
      std::cerr << "Incident load failed: " << snapshot.error().message << '\n';
      return 1;
    }
    rows.push_back(
        {export_key_text(summary.export_key),
         analysis::extract_incident_features(
             summary.id, summary.created_utc_milliseconds, **snapshot)});
  }
  const auto written =
      evaluation::write_offline_feature_matrix(destination, std::move(rows));
  if (!written) {
    std::cerr << "Feature export failed: " << written.error().message << '\n';
    return 1;
  }
  std::cout << "Published a label-free direct-v1 feature matrix at "
            << destination.string() << ".\n";
  return 0;
}

int compare(const std::filesystem::path &corpus_path,
            const std::filesystem::path &baseline_path,
            const std::filesystem::path &candidate_path) {
  const auto corpus = evaluation::load_dogfood_corpus(corpus_path);
  if (!corpus || !corpus->manifest.frozen) {
    std::cerr << "Comparison requires one valid frozen corpus.\n";
    return 1;
  }
  const auto baseline =
      evaluation::verify_diagnostic_evaluation_artifact(baseline_path, *corpus);
  const auto candidate = evaluation::verify_diagnostic_evaluation_artifact(
      candidate_path, *corpus);
  if (!baseline || !candidate) {
    std::cerr
        << "Both evaluation directories must independently verify against "
           "the same frozen corpus.\n";
    return 1;
  }
  const auto comparison = evaluation::compare_offline_model_to_baseline(
      baseline->report, candidate->report);
  if (!comparison) {
    std::cerr << "Reports are incomparable: " << comparison.error().message
              << '\n';
    return 1;
  }
  std::cout << std::setprecision(12)
            << "non_inferior=" << (comparison->non_inferior ? 1 : 0) << '\n'
            << "supported_precision_delta="
            << comparison->supported_precision_delta << '\n'
            << "supported_recall_delta=" << comparison->supported_recall_delta
            << '\n'
            << "unknown_abstention_delta="
            << comparison->unknown_abstention_delta << '\n'
            << "top3_contributor_delta=" << comparison->top3_contributor_delta
            << '\n'
            << "false_assertion_delta=" << comparison->false_assertion_delta
            << '\n'
            << "brier_score_delta=" << comparison->brier_score_delta << '\n'
            << "calibration_error_delta=" << comparison->calibration_error_delta
            << '\n';
  return comparison->non_inferior ? 0 : 3;
}

void usage() {
  std::cerr
      << "Usage:\n"
      << "  blackbox_offline_ml_tool export-features <archive.sqlite3> "
         "<new-features.tsv>\n"
      << "  blackbox_offline_ml_tool compare <frozen-corpus-directory> "
         "<baseline-evaluation-directory> <candidate-evaluation-directory>\n";
}

} // namespace

int main(const int argc, char **argv) {
  try {
    if (argc == 4 && std::string_view{argv[1]} == "export-features") {
      return export_features(argv[2], argv[3]);
    }
    if (argc == 5 && std::string_view{argv[1]} == "compare") {
      return compare(argv[2], argv[3], argv[4]);
    }
    usage();
    return 2;
  } catch (const std::exception &exception) {
    std::cerr << "Offline model tool failed: " << exception.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "Offline model tool failed with an unknown error.\n";
    return 1;
  }
}
