#include "app/archive_maintenance_service.hpp"
#include "app/incident_viewer_service.hpp"
#include "storage/test_incident.hpp"
#include "core/filesystem_text.hpp"
#include "telemetry/collector.hpp"
#include "telemetry/mock/mock_telemetry_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>

namespace app = blackbox::app;
namespace core = blackbox::core;
namespace storage = blackbox::storage;
using namespace std::chrono_literals;

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<std::uint64_t> sequence{};
        path = std::filesystem::temp_directory_path() /
               ("blackbox-maintenance-test-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path{};
};

template <typename Predicate> bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

} // namespace

TEST_CASE("privacy purge discards recoverable evidence before another retry",
          "[app][archive][privacy][recovery]") {
    class Source final : public core::IIncidentWorkSource {
    public:
        std::shared_ptr<const core::IncidentSnapshot> item =
            storage::test::representative_incident();
        std::shared_ptr<const core::IncidentSnapshot> try_pop() noexcept override {
            return std::exchange(item, {});
        }
        std::shared_ptr<const core::IncidentSnapshot>
        wait_pop(std::stop_token token) noexcept override {
            if (item) return try_pop();
            while (!token.stop_requested())
                std::this_thread::sleep_for(1ms);
            return {};
        }
    } source;
    storage::SqliteIncidentArchive archive{{":memory:"}};
    storage::IncidentWriter writer{source, archive};
    writer.start();
    REQUIRE(wait_until([&] { return writer.diagnostics().recoverable_incident_available; }));
    writer.stop();
    REQUIRE(archive.open());
    app::ArchiveMaintenanceService service{archive, writer};
    service.start();
    TemporaryDirectory temporary;
    const auto exported = temporary.path / std::filesystem::path{u8"failed \u6d4b\u8bd5.sqlite3"};
    service.export_failed(exported);
    REQUIRE(wait_until(
        [&] { return service.snapshot()->status.starts_with("Recoverable incident exported"); }));
    storage::ArchiveConfiguration exported_config{exported};
    exported_config.open_mode = storage::ArchiveOpenMode::read_only;
    storage::SqliteIncidentArchive exported_archive{exported_config};
    REQUIRE(exported_archive.open());
    CHECK(*exported_archive.incident_count() == 1U);
    CHECK(writer.recoverable_incident());
    service.restore(exported, temporary.path / "safety.sqlite3");
    REQUIRE(wait_until([&] { return !service.boundary_pending(); }));
    CHECK(writer.recoverable_incident());
    CHECK(*archive.incident_count() == 0U);
    CHECK_FALSE(std::filesystem::exists(temporary.path / "safety.sqlite3"));
    service.purge_all();
    REQUIRE(wait_until([&] { return !service.boundary_pending(); }));
    CHECK_FALSE(writer.recoverable_incident());
    CHECK_FALSE(writer.retry_recoverable());
    CHECK(*archive.incident_count() == 0U);
    service.stop();
}

TEST_CASE("summary export writes Unicode destinations and preserves existing files",
          "[app][viewer][export][unicode]") {
    TemporaryDirectory temporary;
    storage::SqliteIncidentArchive archive{{":memory:"}};
    REQUIRE(archive.open());
    app::IncidentViewerService viewer{archive};
    auto content = std::make_shared<blackbox::ui::IncidentViewerContent>();
    content->detail.emplace();
    content->detail->id = 42;
    content->detail->label = "Private label";
    content->detail->note = "Private note";
    const auto path = temporary.path / std::filesystem::path{u8"summary \u00e9 \u6d4b\u8bd5.txt"};
    viewer.start();
    REQUIRE(viewer.export_summary(content, path, false));
    REQUIRE(wait_until([&] { return viewer.queue_diagnostics().completed_mutations == 1U; }));
    std::ifstream input{path, std::ios::binary};
    const std::string original{std::istreambuf_iterator<char>{input}, {}};
    CHECK(original.find("42") != std::string::npos);
    CHECK(original.find("Private") == std::string::npos);
    input.close();
    REQUIRE(viewer.export_summary(content, path, true));
    REQUIRE(wait_until([&] { return viewer.queue_diagnostics().failed_mutations == 1U; }));
    viewer.stop();
    std::ifstream reread{path, std::ios::binary};
    const std::string retained{std::istreambuf_iterator<char>{reread}, {}};
    CHECK(retained == original);
}

TEST_CASE("verified backups publish Unicode paths and refuse occupied staging",
          "[app][archive][backup][unicode]") {
    TemporaryDirectory temporary;
    const auto folder = temporary.path / std::filesystem::path{u8"\u6d4b\u8bd5 \u00e9"};
    storage::SqliteIncidentArchive archive{{folder / "active.sqlite3"}};
    REQUIRE(archive.open());
    REQUIRE(archive.store(*storage::test::representative_incident()));
    const auto backup = folder / "backup.sqlite3";
    REQUIRE(archive.backup_to(backup));
    CHECK_FALSE(std::filesystem::exists(folder / "backup.sqlite3.partial"));
    storage::ArchiveConfiguration config{backup};
    config.open_mode = storage::ArchiveOpenMode::read_only;
    storage::SqliteIncidentArchive check{config};
    REQUIRE(check.open());
    CHECK(*check.incident_count() == 1U);
    CHECK_FALSE(archive.backup_to(backup));
    std::ofstream occupied{folder / "another.sqlite3.partial"};
    occupied << "preserve";
    occupied.close();
    CHECK_FALSE(archive.backup_to(folder / "another.sqlite3"));
    CHECK_FALSE(std::filesystem::exists(folder / "another.sqlite3"));
}

TEST_CASE("purge empties a stopped writer queue and invalidates the viewer after queued edits",
          "[app][archive][privacy][lifecycle]") {
    class Source final : public core::IIncidentWorkSource {
    public:
        std::shared_ptr<const core::IncidentSnapshot> item =
            storage::test::representative_incident();
        std::shared_ptr<const core::IncidentSnapshot> try_pop() noexcept override {
            return std::exchange(item, {});
        }
        std::shared_ptr<const core::IncidentSnapshot> wait_pop(std::stop_token) noexcept override {
            return try_pop();
        }
    } source;
    storage::SqliteIncidentArchive archive{{":memory:"}};
    REQUIRE(archive.open());
    const auto stored = archive.store(*storage::test::representative_incident());
    REQUIRE(stored);
    storage::IncidentWriter writer{source, archive};
    app::IncidentViewerService viewer{archive};
    viewer.start();
    viewer.request_detail(*stored);
    REQUIRE(wait_until([&] { return viewer.snapshot()->detail.has_value(); }));
    REQUIRE(viewer.update_annotation(*stored, "queued edit", "old evidence", {}, {}));
    app::ArchiveMaintenanceService service{archive, writer};
    service.attach_lifecycle(nullptr, nullptr, &viewer);
    service.start();
    service.purge_all();
    REQUIRE(wait_until([&] { return !service.boundary_pending(); }));
    CHECK(viewer.queue_diagnostics().completed_mutations == 1U);
    CHECK_FALSE(viewer.snapshot()->detail);
    CHECK_FALSE(source.try_pop());
    CHECK(*archive.incident_count() == 0U);
    CHECK(writer.diagnostics().cancelled == 1U);
    service.stop();
    viewer.stop();
}

TEST_CASE("purge cancels pending capture history and restarts recording in a new epoch",
          "[app][archive][privacy][lifecycle]") {
    namespace telemetry = blackbox::telemetry;
    core::SystemMonotonicClock clock;
    telemetry::mock::MockTelemetryProvider provider{clock};
    telemetry::RecorderConfiguration config{};
    config.sample_interval = 100ms;
    config.history_duration = 2s;
    config.incident_pre_window = 1s;
    config.incident_post_window = 1s;
    const auto validated = telemetry::validate_recorder_configuration(config);
    REQUIRE(validated);
    telemetry::TelemetryCollector collector{provider, clock, *validated};
    storage::SqliteIncidentArchive archive{{":memory:"}};
    REQUIRE(archive.open());
    storage::IncidentWriter writer{collector.incident_work_source(), archive};
    app::ArchiveMaintenanceService service{archive, writer};
    service.attach_lifecycle(&collector, nullptr, nullptr);
    service.start();
    writer.start();
    collector.start();
    REQUIRE(wait_until([&] { return collector.diagnostics().collection_count > 0U; }));
    REQUIRE(collector.request_incident_capture() == core::IncidentCaptureRequestResult::started);
    service.purge_all();
    REQUIRE(wait_until([&] { return !service.boundary_pending(); }));
    CHECK(collector.running());
    CHECK(service.snapshot()->content_epoch == 1U);
    REQUIRE(wait_until([&] { return collector.diagnostics().collection_count >= 12U; }));
    collector.stop();
    writer.stop();
    service.stop();
    CHECK(*archive.incident_count() == 0U);
    CHECK(collector.incident_capture_status().incidents_completed == 0U);
    CHECK_FALSE(writer.recoverable_incident());
}

TEST_CASE("archive maintenance executes guided jobs away from the caller",
          "[app][archive][maintenance][worker][recovery]") {
    TemporaryDirectory temporary;
    storage::SqliteIncidentArchive archive{{temporary.path / "active.sqlite3"}};
    REQUIRE(archive.open().has_value());
    REQUIRE(archive.store(*storage::test::representative_incident()).has_value());
    REQUIRE(archive.store(*storage::test::representative_incident()).has_value());
    core::IncidentCaptureCoordinator source{2U};
    storage::IncidentWriter writer{source, archive};
    app::ArchiveMaintenanceService service{archive, writer};
    service.start();

    service.refresh();
    REQUIRE(wait_until([&] {
        const auto state = service.snapshot();
        return !state->busy && state->healthy && state->incident_count == 2U;
    }));

    const auto backup = temporary.path / "backup.sqlite3";
    service.backup(backup);
    REQUIRE(wait_until(
        [&] { return service.snapshot()->status.starts_with("Verified backup created"); }));
    CHECK(std::filesystem::exists(backup));

    service.retain_newest(1U);
    REQUIRE(wait_until([&] {
        const auto state = service.snapshot();
        return !state->busy && state->incident_count == 1U;
    }));

    const auto dataset = temporary.path / "dataset";
    service.export_dataset(dataset);
    REQUIRE(wait_until(
        [&] { return service.snapshot()->status.starts_with("Evidence dataset exported"); }));
    CHECK(std::filesystem::exists(dataset / "manifest.json"));

    service.purge_all();
    REQUIRE(wait_until([&] {
        const auto state = service.snapshot();
        return !state->busy && state->incident_count == 0U;
    }));
    service.stop();
}
