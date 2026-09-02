#include "storage/incident_dataset.hpp"
#include "storage/test_incident.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

namespace core = blackbox::core;
namespace storage = blackbox::storage;

namespace {

class DatasetFixture final {
public:
    DatasetFixture() {
        static std::atomic<std::uint64_t> sequence{};
        const auto stem = "blackbox-dataset-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(sequence.fetch_add(1U));
        archive = std::filesystem::temp_directory_path() / (stem + ".sqlite3");
        dataset = std::filesystem::temp_directory_path() / stem;
    }
    ~DatasetFixture() {
        std::error_code ignored;
        std::filesystem::remove(archive, ignored);
        std::filesystem::remove(archive.string() + "-wal", ignored);
        std::filesystem::remove(archive.string() + "-shm", ignored);
        std::filesystem::remove_all(dataset, ignored);
    }
    std::filesystem::path archive{};
    std::filesystem::path dataset{};
};

[[nodiscard]] std::string read_all(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

[[nodiscard]] bool same_telemetry(const core::IncidentSnapshot& left,
                                  const core::IncidentSnapshot& right) {
    return left.header() == right.header() &&
           std::ranges::equal(left.system_samples(), right.system_samples()) &&
           std::ranges::equal(left.process_metadata(), right.process_metadata()) &&
           std::ranges::equal(left.process_samples(), right.process_samples()) &&
           std::ranges::equal(left.system_events(), right.system_events());
}

} // namespace

TEST_CASE("offline dataset round-trips classification without changing telemetry",
          "[storage][dataset][roundtrip]") {
    DatasetFixture fixture;
    storage::SqliteIncidentArchive archive{{fixture.archive}};
    REQUIRE(archive.open().has_value());
    const auto base = storage::test::representative_incident();
    const auto identity = base->process_metadata().front().identity;
    core::SystemEvent lifecycle{};
    lifecycle.observed_at = base->header().window.event_time;
    lifecycle.source = core::SystemEventSource::process;
    lifecycle.kind = core::SystemEventKind::process_started;
    lifecycle.has_process_identity = true;
    lifecycle.process_pid = identity.pid;
    lifecycle.process_creation_token = identity.creation_token;
    std::vector<core::SystemEvent> event_rows{base->system_events().begin(),
                                              base->system_events().end()};
    event_rows.push_back(lifecycle);
    const auto original = std::make_shared<const core::IncidentSnapshot>(
        base->header(),
        std::vector<core::IncidentSystemSample>{base->system_samples().begin(),
                                                base->system_samples().end()},
        std::vector<core::IncidentProcessInfo>{base->process_metadata().begin(),
                                               base->process_metadata().end()},
        std::vector<core::IncidentProcessSample>{base->process_samples().begin(),
                                                 base->process_samples().end()},
        std::move(event_rows));
    const auto id = archive.store(*original);
    REQUIRE(id.has_value());
    const storage::IncidentAnnotation exported{
        "Private label", "C:\\Users\\private\\secret.exe was visible",
        storage::IncidentUserFeedback::noticed_problem,
        storage::IncidentCategory::game_stutter};
    REQUIRE(archive.update_annotation(*id, exported).has_value());
    REQUIRE(archive.update_recurring_group_override(
                *id, "private recurring symptom").has_value());

    const auto exported_statistics = storage::export_incident_dataset(
        archive, fixture.dataset);
    REQUIRE(exported_statistics.has_value());
    CHECK(exported_statistics->incidents == 1U);
    CHECK(exported_statistics->system_samples == original->system_samples().size());
    CHECK(exported_statistics->process_samples == original->process_samples().size());
    CHECK(exported_statistics->system_events == original->system_events().size());

    std::string all_files;
    for (const auto& entry : std::filesystem::directory_iterator{fixture.dataset}) {
        all_files += read_all(entry.path());
    }
    CHECK(all_files.find("Private label") == std::string::npos);
    CHECK(all_files.find("C:\\Users\\private") == std::string::npos);
    CHECK(all_files.find("fixture.exe") == std::string::npos);
    CHECK(all_files.find("private recurring symptom") == std::string::npos);
    CHECK(all_files.find("blackbox-offline-dataset") != std::string::npos);
    CHECK(all_files.find("bytes/second") != std::string::npos);
    CHECK(all_files.find("foreground process identity") != std::string::npos);
    CHECK(all_files.find("foreground application identity") != std::string::npos);
    CHECK(all_files.find("987654") == std::string::npos);
    CHECK(all_files.find("456789") == std::string::npos);
    CHECK(all_files.find("system_events.tsv") == std::string::npos);
    CHECK(read_all(fixture.dataset / "system_events.tsv").find("\t1014\t9\t\n") !=
          std::string::npos);
    CHECK(read_all(fixture.dataset / "system_events.tsv").find(
              "\t10\t31\t0\t0\t0\t0\n") != std::string::npos);

    const storage::IncidentAnnotation changed{
        exported.label, exported.note,
        storage::IncidentUserFeedback::did_not_notice_problem,
        storage::IncidentCategory::audio};
    REQUIRE(archive.update_annotation(*id, changed).has_value());
    const auto before_import = archive.load(*id);
    REQUIRE(before_import.has_value());
    const auto imported = storage::import_incident_dataset_classifications(
        archive, fixture.dataset);
    REQUIRE(imported.has_value());
    CHECK(imported->incidents == 1U);
    CHECK(imported->classifications_updated == 1U);
    CHECK(*archive.annotation(*id) == exported);
    const auto after_import = archive.load(*id);
    REQUIRE(after_import.has_value());
    CHECK(same_telemetry(**before_import, **after_import));

    const auto history = archive.classification_history(*id);
    REQUIRE(history.has_value());
    REQUIRE_FALSE(history->empty());
    CHECK(history->back().origin == storage::ClassificationChangeOrigin::dataset_import);
    const auto repeated = storage::import_incident_dataset_classifications(
        archive, fixture.dataset);
    REQUIRE(repeated.has_value());
    CHECK(repeated->classifications_updated == 0U);
    CHECK(archive.classification_history(*id)->size() == history->size());
}

TEST_CASE("dataset export refuses an existing destination",
          "[storage][dataset][safety]") {
    DatasetFixture fixture;
    storage::SqliteIncidentArchive archive{{fixture.archive}};
    REQUIRE(archive.open().has_value());
    REQUIRE(archive.store(*storage::test::representative_incident()).has_value());
    REQUIRE(std::filesystem::create_directories(fixture.dataset));
    std::ofstream{fixture.dataset / "keep.txt"} << "keep";
    const auto exported = storage::export_incident_dataset(archive, fixture.dataset);
    REQUIRE_FALSE(exported.has_value());
    CHECK(read_all(fixture.dataset / "keep.txt") == "keep");
}
