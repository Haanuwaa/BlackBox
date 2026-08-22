#include "storage/incident_archive.hpp"
#include "storage/test_incident.hpp"

#include <sqlite3.h>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <vector>

namespace core = blackbox::core;
namespace storage = blackbox::storage;
using namespace std::chrono_literals;

namespace {

class TemporaryArchive final {
public:
    TemporaryArchive() {
        static std::atomic<std::uint64_t> sequence{};
        path = std::filesystem::temp_directory_path() /
               ("blackbox-storage-test-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(sequence.fetch_add(1U)) + ".sqlite3");
    }
    ~TemporaryArchive() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
        std::filesystem::remove_all(path.string() + "-directory", ignored);
    }
    std::filesystem::path path{};
};

[[nodiscard]] bool same_incident(const core::IncidentSnapshot& left,
                                 const core::IncidentSnapshot& right) {
    return left.header() == right.header() &&
           std::ranges::equal(left.system_samples(), right.system_samples()) &&
           std::ranges::equal(left.process_metadata(), right.process_metadata()) &&
           std::ranges::equal(left.process_samples(), right.process_samples()) &&
           std::ranges::equal(left.system_events(), right.system_events());
}

void execute(sqlite3* database, const char* sql) {
    char* message = nullptr;
    const auto result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    const std::string detail = message == nullptr ? std::string{} : std::string{message};
    if (message != nullptr) sqlite3_free(message);
    INFO(detail);
    REQUIRE(result == SQLITE_OK);
}

[[nodiscard]] std::vector<char> read_bytes(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("in-memory archive creates schema and round-trips every incident value",
          "[storage][sqlite][roundtrip]") {
    storage::SqliteIncidentArchive archive{{std::filesystem::path{":memory:"}}};
    REQUIRE(archive.open().has_value());
    REQUIRE(archive.schema_version().has_value());
    CHECK(*archive.schema_version() == storage::current_schema_version);

    const auto original = storage::test::representative_incident();
    const auto stored = archive.store(*original);
    REQUIRE(stored.has_value());
    REQUIRE(archive.incident_count().has_value());
    CHECK(*archive.incident_count() == 1U);
    const auto discovered = archive.list();
    REQUIRE(discovered.has_value());
    REQUIRE(discovered->size() == 1U);
    CHECK(discovered->front().id == *stored);
    CHECK(discovered->front().capture_sequence ==
          std::numeric_limits<std::uint64_t>::max());

    const auto loaded = archive.load(*stored);
    REQUIRE(loaded.has_value());
    CHECK(same_incident(*original, **loaded));
}

TEST_CASE("direct V1 round trips the complete automatic symptom signal range",
          "[storage][sqlite][roundtrip][automatic][storage]") {
    storage::SqliteIncidentArchive archive{{std::filesystem::path{":memory:"}}};
    REQUIRE(archive.open().has_value());
    const auto base = storage::test::representative_incident();
    auto header = base->header();
    header.window.automatic_trigger_count = 1U;
    header.window.automatic_resource = core::AutomaticIncidentResource::disk;
    header.window.automatic_signal =
        core::AutomaticIncidentSignal::storage_io_retry;
    const core::SystemEvent recovery{
        .observed_at = header.window.event_time,
        .source = core::SystemEventSource::storage,
        .kind = core::SystemEventKind::storage_io_retry,
        .level = core::SystemEventLevel::warning,
        .native_event_id = 153U};
    const core::IncidentSnapshot original{
        header,
        {base->system_samples().begin(), base->system_samples().end()},
        {base->process_metadata().begin(), base->process_metadata().end()},
        {base->process_samples().begin(), base->process_samples().end()},
        {recovery}};

    const auto stored = archive.store(original);
    REQUIRE(stored.has_value());
    const auto loaded = archive.load(*stored);
    REQUIRE(loaded.has_value());
    CHECK(same_incident(original, **loaded));
}

TEST_CASE("direct V1 round trips process lifecycle identity evidence",
          "[storage][sqlite][roundtrip][process][privacy]") {
    storage::SqliteIncidentArchive archive{{std::filesystem::path{":memory:"}}};
    REQUIRE(archive.open().has_value());
    const auto base = storage::test::representative_incident();
    const auto identity = base->process_metadata().front().identity;
    core::SystemEvent started{};
    started.observed_at = base->header().window.event_time;
    started.source = core::SystemEventSource::process;
    started.kind = core::SystemEventKind::process_started;
    started.has_process_identity = true;
    started.process_pid = identity.pid;
    started.process_creation_token = identity.creation_token;
    const core::IncidentSnapshot original{
        base->header(),
        {base->system_samples().begin(), base->system_samples().end()},
        {base->process_metadata().begin(), base->process_metadata().end()},
        {base->process_samples().begin(), base->process_samples().end()},
        {started}};

    const auto stored = archive.store(original);
    REQUIRE(stored.has_value());
    const auto loaded = archive.load(*stored);
    REQUIRE(loaded.has_value());
    CHECK(same_incident(original, **loaded));
}

TEST_CASE("online backup and validated restore preserve a safety copy",
          "[storage][sqlite][backup][restore][recovery]") {
    TemporaryArchive active_file;
    TemporaryArchive backup_file;
    TemporaryArchive safety_file;
    storage::SqliteIncidentArchive active{{active_file.path}};
    REQUIRE(active.open().has_value());
    REQUIRE(active.store(*storage::test::representative_incident()).has_value());
    REQUIRE(active.backup_to(backup_file.path).has_value());
    REQUIRE(std::filesystem::exists(backup_file.path));

    REQUIRE(active.store(*storage::test::representative_incident()).has_value());
    REQUIRE(*active.incident_count() == 2U);
    REQUIRE(active.restore_from(backup_file.path, safety_file.path).has_value());
    CHECK(*active.incident_count() == 1U);
    CHECK(std::filesystem::exists(safety_file.path));

    storage::SqliteIncidentArchive safety{{safety_file.path}};
    REQUIRE(safety.open().has_value());
    CHECK(*safety.incident_count() == 2U);
    CHECK_FALSE(active.backup_to(backup_file.path));
}

TEST_CASE("restore rejects a corrupt source without changing the archive",
          "[storage][sqlite][restore][corrupt]") {
    TemporaryArchive active_file;
    TemporaryArchive corrupt_file;
    TemporaryArchive safety_file;
    storage::SqliteIncidentArchive active{{active_file.path}};
    REQUIRE(active.open().has_value());
    REQUIRE(active.store(*storage::test::representative_incident()).has_value());
    {
        std::ofstream output{corrupt_file.path, std::ios::binary};
        output << "not a sqlite archive";
    }
    const auto restored = active.restore_from(corrupt_file.path, safety_file.path);
    REQUIRE_FALSE(restored.has_value());
    CHECK(*active.incident_count() == 1U);
    CHECK_FALSE(std::filesystem::exists(safety_file.path));
}

TEST_CASE("restore rejects an incompatible direct-v1 layout without changing the archive",
          "[storage][sqlite][restore][schema-v1]") {
    TemporaryArchive active_file;
    TemporaryArchive source_file;
    TemporaryArchive safety_file;
    {
        storage::SqliteIncidentArchive source{{source_file.path}};
        REQUIRE(source.open().has_value());
        source.close();
    }
    sqlite3* source_database = nullptr;
    REQUIRE(sqlite3_open(source_file.path.string().c_str(), &source_database) == SQLITE_OK);
    execute(source_database, "ALTER TABLE incidents ADD COLUMN alternate_value INTEGER");
    sqlite3_close(source_database);
    const auto source_before = read_bytes(source_file.path);

    storage::SqliteIncidentArchive active{{active_file.path}};
    REQUIRE(active.open().has_value());
    REQUIRE(active.store(*storage::test::representative_incident()).has_value());
    const auto restored = active.restore_from(source_file.path, safety_file.path);
    REQUIRE_FALSE(restored.has_value());
    CHECK(restored.error().code == storage::StorageErrorCode::corrupt);
    CHECK(*active.incident_count() == 1U);
    CHECK_FALSE(std::filesystem::exists(safety_file.path));
    CHECK(read_bytes(source_file.path) == source_before);
}

TEST_CASE("file archive discovers committed incidents after restart",
          "[storage][sqlite][restart]") {
    TemporaryArchive temporary;
    std::int64_t incident_id = 0;
    {
        storage::SqliteIncidentArchive archive{{temporary.path}};
        REQUIRE(archive.open().has_value());
        const auto stored = archive.store(*storage::test::representative_incident());
        REQUIRE(stored.has_value());
        incident_id = *stored;
        archive.close();
    }
    {
        storage::SqliteIncidentArchive archive{{temporary.path}};
        REQUIRE(archive.open().has_value());
        const auto incidents = archive.list();
        REQUIRE(incidents.has_value());
        REQUIRE(incidents->size() == 1U);
        CHECK(incidents->front().id == incident_id);
        const auto loaded = archive.load(incident_id);
        REQUIRE(loaded.has_value());
        CHECK((*loaded)->header().window.sequence ==
              std::numeric_limits<std::uint64_t>::max());
    }
}

TEST_CASE("read-only archive access preserves evidence and cannot create or store",
          "[storage][sqlite][read-only][evidence]") {
    TemporaryArchive temporary;
    std::int64_t incident_id{};
    {
        storage::SqliteIncidentArchive writer{{temporary.path}};
        REQUIRE(writer.open().has_value());
        const auto stored = writer.store(*storage::test::representative_incident());
        REQUIRE(stored.has_value());
        incident_id = *stored;
        writer.close();
    }
    const auto before = read_bytes(temporary.path);
    REQUIRE_FALSE(before.empty());
    storage::SqliteIncidentArchive reader{storage::ArchiveConfiguration{
        .path = temporary.path,
        .open_mode = storage::ArchiveOpenMode::read_only}};
    REQUIRE(reader.open().has_value());
    REQUIRE(reader.schema_version().has_value());
    CHECK(*reader.schema_version() == storage::current_schema_version);
    const auto listed = reader.list();
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1U);
    REQUIRE(reader.load(incident_id).has_value());
    CHECK_FALSE(reader.store(*storage::test::representative_incident()).has_value());
    reader.close();
    CHECK(read_bytes(temporary.path) == before);

    TemporaryArchive missing;
    storage::SqliteIncidentArchive absent{storage::ArchiveConfiguration{
        .path = missing.path,
        .open_mode = storage::ArchiveOpenMode::read_only}};
    CHECK_FALSE(absent.open().has_value());
    CHECK_FALSE(std::filesystem::exists(missing.path));
}

TEST_CASE("explicit retention and privacy purge are transactional and cascade incident data",
          "[storage][sqlite][retention][privacy]") {
    TemporaryArchive temporary;
    storage::SqliteIncidentArchive archive{{temporary.path}};
    REQUIRE(archive.open().has_value());
    std::vector<std::int64_t> identities;
    for (std::size_t index = 0U; index < 4U; ++index) {
        const auto stored = archive.store(*storage::test::representative_incident());
        REQUIRE(stored.has_value());
        identities.push_back(*stored);
    }

    CHECK_FALSE(archive.apply_retention({}).has_value());
    CHECK_FALSE(archive.apply_retention(
        storage::ArchiveRetentionPolicy{.maximum_incidents = 0U}).has_value());
    const auto retained = archive.apply_retention(
        storage::ArchiveRetentionPolicy{.maximum_incidents = 2U});
    REQUIRE(retained.has_value());
    CHECK(retained->incidents_deleted == 2U);
    CHECK(retained->incidents_remaining == 2U);
    CHECK_FALSE(archive.load(identities.front()).has_value());
    CHECK(archive.load(identities.back()).has_value());

    REQUIRE(archive.reset_feedback_profile().has_value());

    const auto purged = archive.purge_all_incidents();
    REQUIRE(purged.has_value());
    CHECK(purged->incidents_deleted == 2U);
    CHECK(purged->incidents_remaining == 0U);
    REQUIRE(archive.incident_count().has_value());
    CHECK(*archive.incident_count() == 0U);
    REQUIRE(archive.process_profile_storage_statistics().has_value());
    CHECK(archive.process_profile_storage_statistics()->identity_count == 0U);
    const auto post_purge_incident =
        archive.store(*storage::test::representative_incident());
    REQUIRE(post_purge_incident.has_value());
    const auto post_purge_feedback =
        archive.feedback_calibration_context(*post_purge_incident);
    REQUIRE(post_purge_feedback.has_value());
    CHECK(post_purge_feedback->profile_revision == 0U);
    CHECK(post_purge_feedback->reset_after_utc_milliseconds == 0);
    CHECK_FALSE(post_purge_feedback->rollback_available);
}

TEST_CASE("pre-release archive rejects an unversioned non-empty database",
          "[storage][sqlite][schema][recovery]") {
    TemporaryArchive temporary;
    sqlite3* database = nullptr;
    REQUIRE(sqlite3_open(temporary.path.string().c_str(), &database) == SQLITE_OK);
    execute(database, "CREATE TABLE unrelated(value TEXT)");
    sqlite3_close(database);

    storage::SqliteIncidentArchive archive{{temporary.path}};
    const auto opened = archive.open();
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == storage::StorageErrorCode::invalid_schema);
    CHECK(std::filesystem::exists(temporary.path));
}
TEST_CASE("recurring feature cache and manual overrides round-trip and replace",
          "[storage][sqlite][recurring]") {
    TemporaryArchive temporary;
    std::int64_t incident_id{};
    {
        storage::SqliteIncidentArchive archive{{temporary.path}};
        REQUIRE(archive.open().has_value());
        const auto stored = archive.store(*storage::test::representative_incident());
        REQUIRE(stored.has_value());
        incident_id = *stored;
        storage::StoredIncidentFeatureCache feature{};
        feature.incident_id = incident_id;
        feature.feature_version = 1;
        feature.values = {0.1, 0.2, 0.3};
        feature.available = {1U, 0U, 1U};
        REQUIRE(archive.store_incident_features(std::span{&feature, 1U}).has_value());
        REQUIRE(archive.update_recurring_group_override(
                    incident_id, "same symptom").has_value());
        REQUIRE(archive.update_annotation(
                    incident_id,
                    {"confirmed", "", storage::IncidentUserFeedback::noticed_problem,
                     storage::IncidentCategory::game_stutter}).has_value());
        const auto recurring = archive.recurring_incidents();
        REQUIRE(recurring.has_value());
        REQUIRE(recurring->size() == 1U);
        CHECK(recurring->front().override_group == "same symptom");
        CHECK(recurring->front().user_feedback ==
              storage::IncidentUserFeedback::noticed_problem);
        CHECK(recurring->front().category ==
              storage::IncidentCategory::game_stutter);
        REQUIRE(recurring->front().cached_feature.has_value());
        CHECK(recurring->front().cached_feature->feature_version == 1);
        CHECK(recurring->front().cached_feature->values == feature.values);
        CHECK(recurring->front().cached_feature->available == feature.available);

        feature.feature_version = 2;
        feature.values = {0.9, 0.8};
        feature.available = {1U, 1U};
        REQUIRE(archive.store_incident_features(std::span{&feature, 1U}).has_value());
        CHECK(archive.update_recurring_group_override(
            incident_id, std::string(storage::maximum_recurring_group_override_bytes + 1U,
                                     'x')).error().code == storage::StorageErrorCode::invalid_data);
    }
    {
        storage::SqliteIncidentArchive archive{{temporary.path}};
        REQUIRE(archive.open().has_value());
        CHECK(*archive.recurring_group_override(incident_id) == "same symptom");
        const auto recurring = archive.recurring_incidents();
        REQUIRE(recurring.has_value());
        REQUIRE(recurring->front().cached_feature.has_value());
        CHECK(recurring->front().cached_feature->feature_version == 2);
        CHECK(recurring->front().cached_feature->values ==
              std::vector<double>{0.9, 0.8});
    }
}

TEST_CASE("classification history is change-only and bounded",
          "[storage][sqlite][classification][bounded]") {
    storage::SqliteIncidentArchive archive{{std::filesystem::path{":memory:"}}};
    REQUIRE(archive.open().has_value());
    const auto id = archive.store(*storage::test::representative_incident());
    REQUIRE(id.has_value());
    REQUIRE(archive.update_annotation(*id, {"label", "note"}).has_value());
    REQUIRE(archive.classification_history(*id)->size() == 1U);
    for (std::size_t index = 0;
         index < storage::maximum_incident_classification_history + 10U; ++index) {
        const auto category = index % 2U == 0U
            ? storage::IncidentCategory::system_freeze
            : storage::IncidentCategory::network;
        REQUIRE(archive.update_annotation(
            *id, {"label", "note", storage::IncidentUserFeedback::noticed_problem,
                  category}).has_value());
    }
    const auto history = archive.classification_history(*id);
    REQUIRE(history.has_value());
    CHECK(history->size() == storage::maximum_incident_classification_history);
}

TEST_CASE("feedback calibration history is bounded answered prior evidence only",
          "[storage][sqlite][feedback][profile]") {
    storage::SqliteIncidentArchive archive{{std::filesystem::path{":memory:"}}};
    REQUIRE(archive.open().has_value());
    std::vector<std::int64_t> ids;
    for (std::size_t index = 0U; index < 6U; ++index) {
        const auto id = archive.store(*storage::test::representative_incident());
        REQUIRE(id.has_value());
        ids.push_back(*id);
    }
    for (std::size_t index = 0U; index < 4U; ++index) {
        const auto feedback = index == 1U
            ? storage::IncidentUserFeedback::noticed_problem
            : storage::IncidentUserFeedback::did_not_notice_problem;
        REQUIRE(archive.update_annotation(
            ids[index], {"", "", feedback,
                         storage::IncidentCategory::unknown}).has_value());
    }

    const auto context = archive.feedback_calibration_context(ids[4]);
    REQUIRE(context.has_value());
    CHECK(context->incident_id == ids[4]);
    CHECK(context->incident_utc_milliseconds > 0);
    CHECK(context->profile_revision == 0U);
    CHECK(context->reset_after_utc_milliseconds == 0);
    CHECK_FALSE(context->rollback_available);
    REQUIRE(context->history.size() == 4U);
    CHECK(context->history.front().incident_id == ids[3]);
    CHECK(context->history.back().incident_id == ids[0]);
    CHECK(context->history[2].feedback ==
          storage::IncidentUserFeedback::noticed_problem);
    for (const auto& observation : context->history) {
        CHECK(observation.incident_id != ids[4]);
        CHECK(observation.incident_id != ids[5]);
        CHECK(observation.automatic_resource ==
              core::AutomaticIncidentResource::cpu);
        CHECK(observation.automatic_signal ==
              core::AutomaticIncidentSignal::disk_latency);
    }

    const auto limited = archive.feedback_calibration_context(ids[4], 2U);
    REQUIRE(limited.has_value());
    REQUIRE(limited->history.size() == 2U);
    CHECK(limited->history[0].incident_id == ids[3]);
    CHECK(limited->history[1].incident_id == ids[2]);
    CHECK_FALSE(archive.feedback_calibration_context(ids[4], 0U).has_value());
    CHECK_FALSE(archive.feedback_calibration_context(
        ids[4], storage::maximum_feedback_calibration_observations + 1U));

    const auto reset = archive.reset_feedback_profile();
    REQUIRE(reset.has_value());
    CHECK(reset->revision == 1U);
    CHECK(reset->reset_after_utc_milliseconds > 0);
    CHECK(reset->rollback_available);
    const auto after_reset = archive.feedback_calibration_context(ids[4]);
    REQUIRE(after_reset.has_value());
    CHECK(after_reset->profile_revision == 1U);
    CHECK(after_reset->rollback_available);
    CHECK(after_reset->history.empty());

    const auto rolled_back = archive.rollback_feedback_profile_reset();
    REQUIRE(rolled_back.has_value());
    CHECK(rolled_back->revision == 2U);
    CHECK(rolled_back->reset_after_utc_milliseconds == 0);
    CHECK_FALSE(rolled_back->rollback_available);
    const auto after_rollback = archive.feedback_calibration_context(ids[4]);
    REQUIRE(after_rollback.has_value());
    CHECK(after_rollback->history.size() == 4U);
    CHECK_FALSE(archive.rollback_feedback_profile_reset().has_value());
}

TEST_CASE("executable profile observations persist idempotently and exclude the current incident",
          "[storage][sqlite][profile][roundtrip]") {
    storage::SqliteIncidentArchive archive{{std::filesystem::path{":memory:"}}};
    REQUIRE(archive.open().has_value());
    const auto first = archive.store(*storage::test::representative_incident());
    const auto second = archive.store(*storage::test::representative_incident());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    const storage::ProcessProfileUpdate update{
        "path:c:\\fixture\\fixture.exe", "fixture.exe", 0.75,
        128.0 * 1024.0 * 1024.0, 10'000.0, std::nullopt};
    REQUIRE(archive.store_process_profile_updates(*first, std::span{&update, 1U})
                .has_value());
    REQUIRE(archive.store_process_profile_updates(*first, std::span{&update, 1U})
                .has_value());
    const std::vector<std::string> keys{update.executable_key};
    const auto context = archive.process_profile_context(*second, keys);
    REQUIRE(context.has_value());
    REQUIRE(context->history.size() == 1U);
    CHECK(context->history.front().incident_id == *first);
    CHECK(context->history.front().cpu_fraction == update.cpu_fraction);
    CHECK(context->history.front().working_set_bytes == update.working_set_bytes);
    CHECK(context->history.front().disk_write_bytes_per_second == std::nullopt);
    const auto statistics = archive.process_profile_storage_statistics();
    REQUIRE(statistics.has_value());
    CHECK(statistics->identity_count == 1U);
    CHECK(statistics->observation_count == 1U);
}

TEST_CASE("executable profile history and identity cardinality remain bounded",
          "[storage][sqlite][profile][soak][bounded]") {
    storage::SqliteIncidentArchive archive{{std::filesystem::path{":memory:"}}};
    REQUIRE(archive.open().has_value());

    for (std::size_t batch = 0U; batch < 5U; ++batch) {
        const auto incident_id = archive.store(*storage::test::representative_incident());
        REQUIRE(incident_id.has_value());
        std::vector<storage::ProcessProfileUpdate> updates;
        const auto count = batch < 4U ? 512U : 32U;
        updates.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            const auto identity = batch * 512U + index;
            updates.push_back({"path:c:\\profiles\\process-" +
                                   std::to_string(identity) + ".exe",
                               "process-" + std::to_string(identity) + ".exe",
                               0.1, 1'048'576.0, 1'000.0, 500.0});
        }
        REQUIRE(archive.store_process_profile_updates(*incident_id, updates).has_value());
    }
    auto statistics = archive.process_profile_storage_statistics();
    REQUIRE(statistics.has_value());
    CHECK(statistics->identity_count == storage::maximum_process_profile_identities);
    CHECK(statistics->observation_count <= storage::maximum_process_profile_identities);

    const storage::ProcessProfileUpdate repeated{
        "path:c:\\profiles\\repeated.exe", "repeated.exe", 0.2,
        2'097'152.0, 2'000.0, 1'000.0};
    for (std::size_t index = 0U;
         index < storage::maximum_process_profile_observations_per_identity + 7U;
         ++index) {
        const auto incident_id = archive.store(*storage::test::representative_incident());
        REQUIRE(incident_id.has_value());
        REQUIRE(archive.store_process_profile_updates(
                    *incident_id, std::span{&repeated, 1U}).has_value());
    }
    const auto query_incident = archive.store(*storage::test::representative_incident());
    REQUIRE(query_incident.has_value());
    const std::vector<std::string> repeated_key{repeated.executable_key};
    const auto context = archive.process_profile_context(*query_incident, repeated_key);
    REQUIRE(context.has_value());
    CHECK(context->history.size() ==
          storage::maximum_process_profile_observations_per_identity);
    statistics = archive.process_profile_storage_statistics();
    REQUIRE(statistics.has_value());
    CHECK(statistics->identity_count <= storage::maximum_process_profile_identities);
    CHECK(statistics->observation_count <=
          storage::maximum_process_profile_identities *
              storage::maximum_process_profile_observations_per_identity);
}

TEST_CASE("incident pages search sort paginate and persist bounded annotations",
          "[storage][sqlite][viewer]") {
    TemporaryArchive temporary;
    storage::SqliteIncidentArchive archive{{temporary.path}};
    REQUIRE(archive.open().has_value());
    const auto first = archive.store(*storage::test::representative_incident());
    const auto second = archive.store(*storage::test::representative_incident());
    const auto third = archive.store(*storage::test::representative_incident());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());
    REQUIRE(archive.update_annotation(*first, {"Network", "packet loss at lobby"})
                .has_value());
    REQUIRE(archive.update_annotation(*second, {"CPU", "compiler spike"}).has_value());
    REQUIRE(archive.update_annotation(*third, {"Audio", "brief crackle"}).has_value());
    REQUIRE(archive.update_annotation(
        *first, {"Network", "packet loss at lobby",
                 storage::IncidentUserFeedback::noticed_problem}).has_value());
    CHECK(archive.annotation(*first)->user_feedback ==
          storage::IncidentUserFeedback::noticed_problem);

    storage::IncidentListQuery query{};
    query.limit = 2U;
    query.sort = storage::IncidentListSort::label_ascending;
    const auto first_page = archive.list_page(query);
    REQUIRE(first_page.has_value());
    CHECK(first_page->total_matching == 3U);
    REQUIRE(first_page->incidents.size() == 2U);
    CHECK(first_page->incidents[0].label == "Audio");
    CHECK(first_page->incidents[1].label == "CPU");
    query.offset = 2U;
    const auto second_page = archive.list_page(query);
    REQUIRE(second_page.has_value());
    REQUIRE(second_page->incidents.size() == 1U);
    CHECK(second_page->incidents[0].label == "Network");

    query = {};
    query.search = "PACKET";
    const auto searched = archive.list_page(query);
    REQUIRE(searched.has_value());
    CHECK(searched->total_matching == 1U);
    REQUIRE(searched->incidents.size() == 1U);
    CHECK(searched->incidents.front().id == *first);

    const storage::IncidentAnnotation maximum{
        std::string(storage::maximum_incident_label_bytes, 'l'),
        std::string(storage::maximum_incident_note_bytes, 'n')};
    const auto maximum_updated = archive.update_annotation(*first, maximum);
    const auto maximum_update_message = maximum_updated.has_value()
                                            ? std::string{}
                                            : maximum_updated.error().message;
    INFO(maximum_update_message);
    REQUIRE(maximum_updated.has_value());
    CHECK(*archive.annotation(*first) == maximum);
    const storage::IncidentAnnotation oversized{
        std::string(storage::maximum_incident_label_bytes + 1U, 'x'), {}};
    const auto rejected = archive.update_annotation(*first, oversized);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == storage::StorageErrorCode::invalid_data);
    archive.close();

    storage::SqliteIncidentArchive reopened{{temporary.path}};
    REQUIRE(reopened.open().has_value());
    CHECK(*reopened.annotation(*first) == maximum);
}

TEST_CASE("future and corrupt schemas fail without replacing user data",
          "[storage][sqlite][recovery]") {
    SECTION("future schema") {
        TemporaryArchive temporary;
        sqlite3* database = nullptr;
        REQUIRE(sqlite3_open(temporary.path.string().c_str(), &database) == SQLITE_OK);
        execute(database, "PRAGMA user_version=99");
        sqlite3_close(database);
        storage::SqliteIncidentArchive archive{{temporary.path}};
        const auto opened = archive.open();
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == storage::StorageErrorCode::schema_too_new);
        CHECK(std::filesystem::exists(temporary.path));
    }
    SECTION("incomplete development version one baseline") {
        TemporaryArchive temporary;
        sqlite3* database = nullptr;
        REQUIRE(sqlite3_open(temporary.path.string().c_str(), &database) == SQLITE_OK);
        execute(database, "CREATE TABLE incidents(id INTEGER PRIMARY KEY)");
        execute(database, "PRAGMA user_version=1");
        sqlite3_close(database);
        storage::SqliteIncidentArchive archive{{temporary.path}};
        const auto opened = archive.open();
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == storage::StorageErrorCode::invalid_schema);
        CHECK(std::filesystem::exists(temporary.path));
    }
    SECTION("development version one contributor table without temporal provenance") {
        TemporaryArchive temporary;
        sqlite3* database = nullptr;
        REQUIRE(sqlite3_open(temporary.path.string().c_str(), &database) == SQLITE_OK);
        execute(database, "CREATE TABLE schema_metadata(id INTEGER PRIMARY KEY)");
        execute(database, "CREATE TABLE incidents(id INTEGER PRIMARY KEY)");
        execute(database, "CREATE TABLE feedback_profile_state(id INTEGER PRIMARY KEY)");
        execute(database,
                "CREATE TABLE incident_contributor_feedback("
                "incident_id INTEGER,executable_key TEXT,resource INTEGER,"
                "disposition INTEGER,updated_utc_ms INTEGER)");
        execute(database, "PRAGMA user_version=1");
        sqlite3_close(database);
        storage::SqliteIncidentArchive archive{{temporary.path}};
        const auto opened = archive.open();
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == storage::StorageErrorCode::invalid_schema);
        CHECK(std::filesystem::exists(temporary.path));
    }
    SECTION("development version one contributor table with an alternate extra column") {
        TemporaryArchive temporary;
        sqlite3* database = nullptr;
        REQUIRE(sqlite3_open(temporary.path.string().c_str(), &database) == SQLITE_OK);
        execute(database, "CREATE TABLE schema_metadata(id INTEGER PRIMARY KEY)");
        execute(database, "CREATE TABLE incidents(id INTEGER PRIMARY KEY)");
        execute(database, "CREATE TABLE feedback_profile_state(id INTEGER PRIMARY KEY)");
        execute(database,
                "CREATE TABLE incident_contributor_feedback("
                "incident_id INTEGER,executable_key TEXT,resource INTEGER,"
                "disposition INTEGER,temporal_relationship INTEGER,"
                "updated_utc_ms INTEGER,alternate_value INTEGER)");
        execute(database, "PRAGMA user_version=1");
        sqlite3_close(database);
        storage::SqliteIncidentArchive archive{{temporary.path}};
        const auto opened = archive.open();
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == storage::StorageErrorCode::invalid_schema);
        CHECK(std::filesystem::exists(temporary.path));
    }
    SECTION("corrupt database") {
        TemporaryArchive temporary;
        {
            std::ofstream output{temporary.path, std::ios::binary};
            output << "not a sqlite database";
        }
        const auto original_size = std::filesystem::file_size(temporary.path);
        storage::SqliteIncidentArchive archive{{temporary.path}};
        const auto opened = archive.open();
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == storage::StorageErrorCode::corrupt);
        CHECK(std::filesystem::file_size(temporary.path) == original_size);
    }
}

TEST_CASE("direct-v1 open requires the exact canonical layout and control state",
          "[storage][sqlite][schema-v1][recovery]") {
    const auto alter_and_reject = [](const char* alteration) {
        TemporaryArchive temporary;
        {
            storage::SqliteIncidentArchive archive{{temporary.path}};
            REQUIRE(archive.open().has_value());
            archive.close();
        }
        sqlite3* database = nullptr;
        REQUIRE(sqlite3_open(temporary.path.string().c_str(), &database) == SQLITE_OK);
        execute(database, alteration);
        sqlite3_close(database);
        const auto before = read_bytes(temporary.path);

        storage::SqliteIncidentArchive reopened{{temporary.path}};
        const auto result = reopened.open();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == storage::StorageErrorCode::invalid_schema);
        CHECK(read_bytes(temporary.path) == before);
    };

    SECTION("missing canonical index") {
        alter_and_reject("DROP INDEX idx_incidents_created");
    }
    SECTION("changed table with the same canonical name") {
        alter_and_reject("ALTER TABLE incidents ADD COLUMN alternate_value INTEGER");
    }
    SECTION("extraneous schema object") {
        alter_and_reject("CREATE TABLE alternate_state(value INTEGER)");
    }
    SECTION("wrong application identity") {
        alter_and_reject("PRAGMA application_id=7");
    }
    SECTION("missing required metadata state") {
        alter_and_reject("DELETE FROM schema_metadata");
    }
    SECTION("missing required feedback control state") {
        alter_and_reject("DELETE FROM feedback_profile_state");
    }
}

TEST_CASE("busy full and unavailable archives return explicit recoverable errors",
          "[storage][sqlite][failure]") {
    SECTION("busy transaction") {
        TemporaryArchive temporary;
        storage::ArchiveConfiguration configuration{temporary.path};
        configuration.busy_timeout = 10ms;
        storage::SqliteIncidentArchive archive{configuration};
        REQUIRE(archive.open().has_value());
        sqlite3* blocker = nullptr;
        REQUIRE(sqlite3_open(temporary.path.string().c_str(), &blocker) == SQLITE_OK);
        execute(blocker, "BEGIN IMMEDIATE");
        const auto stored = archive.store(*storage::test::representative_incident());
        REQUIRE_FALSE(stored.has_value());
        CHECK(stored.error().code == storage::StorageErrorCode::busy);
        execute(blocker, "ROLLBACK");
        sqlite3_close(blocker);
        CHECK(*archive.incident_count() == 0U);
    }
    SECTION("size limit rolls back the whole incident") {
        TemporaryArchive temporary;
        storage::ArchiveConfiguration configuration{temporary.path};
        configuration.maximum_bytes = 1U;
        storage::SqliteIncidentArchive archive{configuration};
        REQUIRE(archive.open().has_value());
        const auto stored = archive.store(*storage::test::representative_incident(2U << 20U));
        REQUIRE_FALSE(stored.has_value());
        CHECK(stored.error().code == storage::StorageErrorCode::full);
        CHECK(*archive.incident_count() == 0U);
    }
    SECTION("unavailable path") {
        TemporaryArchive temporary;
        const auto directory_path = std::filesystem::path{temporary.path.string() + "-directory"};
        std::filesystem::create_directories(directory_path);
        storage::SqliteIncidentArchive archive{{directory_path}};
        const auto opened = archive.open();
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == storage::StorageErrorCode::cannot_open);
    }
}

TEST_CASE("explicit contributor attribution round trips as one direct-v1 vote per incident",
          "[storage][sqlite][contributor-feedback][schema-v1]") {
    storage::SqliteIncidentArchive archive{{std::filesystem::path{":memory:"}}};
    REQUIRE(archive.open().has_value());
    const auto first = archive.store(*storage::test::representative_incident());
    const auto second = archive.store(*storage::test::representative_incident());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    const std::string executable_key{"path:c:/program files/example/app.exe"};

    REQUIRE(archive.update_contributor_feedback(
        *first, executable_key, storage::ContributorFeedbackResource::cpu,
        storage::ContributorFeedbackDisposition::confirmed_contributor,
        storage::ContributorFeedbackTemporalRelationship::preceding_activity).has_value());
    REQUIRE(archive.update_contributor_feedback(
        *first, executable_key, storage::ContributorFeedbackResource::cpu,
        storage::ContributorFeedbackDisposition::not_a_contributor,
        storage::ContributorFeedbackTemporalRelationship::post_marker_reaction).has_value());
    const std::vector<std::string> keys{executable_key};
    const auto second_context = archive.contributor_feedback_context(*second, keys);
    REQUIRE(second_context.has_value());
    CHECK(second_context->current.empty());
    REQUIRE(second_context->history.size() == 1U);
    CHECK(second_context->history.front().incident_id == *first);
    CHECK(second_context->history.front().disposition ==
          storage::ContributorFeedbackDisposition::not_a_contributor);
    CHECK(second_context->history.front().temporal_relationship ==
          storage::ContributorFeedbackTemporalRelationship::post_marker_reaction);

    REQUIRE(archive.update_contributor_feedback(
        *second, executable_key, storage::ContributorFeedbackResource::disk,
        storage::ContributorFeedbackDisposition::confirmed_contributor,
        storage::ContributorFeedbackTemporalRelationship::
            marker_spanning_ambiguous).has_value());
    auto current = archive.contributor_feedback_context(*second, keys);
    REQUIRE(current.has_value());
    REQUIRE(current->current.size() == 1U);
    CHECK(current->current.front().resource ==
          storage::ContributorFeedbackResource::disk);
    CHECK(current->current.front().temporal_relationship ==
          storage::ContributorFeedbackTemporalRelationship::
              marker_spanning_ambiguous);
    REQUIRE(archive.update_contributor_feedback(
        *second, executable_key, storage::ContributorFeedbackResource::disk,
        storage::ContributorFeedbackDisposition::unsure,
        storage::ContributorFeedbackTemporalRelationship::
            marker_spanning_ambiguous).has_value());
    current = archive.contributor_feedback_context(*second, keys);
    REQUIRE(current.has_value());
    CHECK(current->current.empty());

    const auto missing = archive.update_contributor_feedback(
        99'999, executable_key, storage::ContributorFeedbackResource::cpu,
        storage::ContributorFeedbackDisposition::confirmed_contributor,
        storage::ContributorFeedbackTemporalRelationship::preceding_activity);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == storage::StorageErrorCode::invalid_data);
}
