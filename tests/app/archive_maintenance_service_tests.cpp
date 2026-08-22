#include "app/archive_maintenance_service.hpp"
#include "storage/test_incident.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

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
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path{};
};

template <typename Predicate>
bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

} // namespace

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
    REQUIRE(wait_until([&] {
        return service.snapshot()->status.starts_with("Verified backup created");
    }));
    CHECK(std::filesystem::exists(backup));

    service.retain_newest(1U);
    REQUIRE(wait_until([&] {
        const auto state = service.snapshot();
        return !state->busy && state->incident_count == 1U;
    }));

    const auto dataset = temporary.path / "dataset";
    service.export_dataset(dataset);
    REQUIRE(wait_until([&] {
        return service.snapshot()->status.starts_with("Evidence dataset exported");
    }));
    CHECK(std::filesystem::exists(dataset / "manifest.json"));

    service.purge_all();
    REQUIRE(wait_until([&] {
        const auto state = service.snapshot();
        return !state->busy && state->incident_count == 0U;
    }));
    service.stop();
}
