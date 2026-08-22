#include "storage/incident_writer.hpp"
#include "storage/incident_archive.hpp"
#include "storage/test_incident.hpp"
#include "telemetry/collector.hpp"
#include "telemetry/mock/mock_telemetry_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace core = blackbox::core;
namespace storage = blackbox::storage;
namespace telemetry = blackbox::telemetry;
namespace mock = blackbox::telemetry::mock;
using namespace std::chrono_literals;

namespace {

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate,
                              const std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

class FailingArchive final : public storage::IIncidentArchive {
public:
    std::expected<std::int64_t, storage::StorageError> store(
        const core::IncidentSnapshot&) noexcept override {
        ++calls;
        return std::unexpected{storage::StorageError{
            storage::StorageErrorCode::busy, 5, "injected busy database"}};
    }
    std::atomic<std::uint64_t> calls{};
};

class CountingArchive final : public storage::IIncidentArchive {
public:
    std::expected<std::int64_t, storage::StorageError> store(
        const core::IncidentSnapshot&) noexcept override {
        return static_cast<std::int64_t>(++calls);
    }
    std::atomic<std::uint64_t> calls{};
};

class RecoveringArchive final : public storage::IIncidentArchive {
public:
    std::expected<std::int64_t, storage::StorageError> store(
        const core::IncidentSnapshot&) noexcept override {
        if (++calls == 1U) {
            return std::unexpected{storage::StorageError{
                storage::StorageErrorCode::busy, 5, "injected transient lock"}};
        }
        return 42;
    }
    std::uint64_t calls{};
};

class FullArchive final : public storage::IIncidentArchive {
public:
    std::expected<std::int64_t, storage::StorageError> store(
        const core::IncidentSnapshot&) noexcept override {
        ++calls;
        return std::unexpected{storage::StorageError{
            storage::StorageErrorCode::full, 13, "injected full database"}};
    }
    std::uint64_t calls{};
};

class ManuallyRecoveringArchive final : public storage::IIncidentArchive {
public:
    std::expected<std::int64_t, storage::StorageError> store(
        const core::IncidentSnapshot&) noexcept override {
        ++calls;
        if (!available.load()) {
            return std::unexpected{storage::StorageError{
                storage::StorageErrorCode::full, 13, "injected unavailable archive"}};
        }
        return 77;
    }
    std::atomic<bool> available{};
    std::atomic<std::uint64_t> calls{};
};

constexpr storage::IncidentWriterConfiguration immediate_retries{
    3U, 0ms, 0ms};

void enqueue(core::IncidentCaptureCoordinator& source) {
    const auto at = core::MonotonicTimePoint{1s};
    REQUIRE(source.request(at, 0s, 0s) == core::IncidentCaptureRequestResult::started);
    REQUIRE(source.try_begin_snapshot(at).has_value());
    source.finish_snapshot(storage::test::representative_incident());
}

} // namespace

TEST_CASE("writer drains immutable work and exposes archive failures",
          "[storage][writer][failure]") {
    core::IncidentCaptureCoordinator source{2U};
    source.start_accepting();
    FailingArchive archive;
    storage::IncidentWriter writer{source, archive, immediate_retries};
    writer.start();
    enqueue(source);
    REQUIRE(wait_until([&writer] { return writer.diagnostics().failed == 1U; }));
    const auto diagnostics = writer.diagnostics();
    CHECK(diagnostics.state == storage::WriterState::degraded);
    CHECK(diagnostics.attempts == 3U);
    CHECK(diagnostics.retry_attempts == 2U);
    CHECK(diagnostics.retry_exhausted == 1U);
    CHECK(diagnostics.last_error_code == storage::StorageErrorCode::busy);
    CHECK(diagnostics.write_timing.samples == 3U);
    CHECK(source.status().queue_size == 0U);
    writer.stop();
}

TEST_CASE("collector continues while asynchronous archive writes fail",
          "[storage][writer][collector]") {
    core::SystemMonotonicClock clock;
    mock::MockTelemetryProvider provider{clock};
    auto values = telemetry::RecorderConfiguration{1ms, 20ms, 10ms};
    values.incident_pre_window = 2ms;
    values.incident_post_window = 2ms;
    const auto configuration = telemetry::validate_recorder_configuration(values);
    REQUIRE(configuration.has_value());
    telemetry::TelemetryCollector collector{provider, clock, *configuration};
    FailingArchive archive;
    storage::IncidentWriter writer{collector.incident_work_source(), archive,
                                   immediate_retries};
    writer.start();
    collector.start();
    REQUIRE(wait_until([&collector] {
        return collector.diagnostics().collection_count >= 5U;
    }));
    const auto before = collector.diagnostics().collection_count;
    REQUIRE(collector.request_incident_capture() ==
            core::IncidentCaptureRequestResult::started);
    REQUIRE(wait_until([&writer] { return writer.diagnostics().failed == 1U; }));
    REQUIRE(wait_until([&collector, before] {
        return collector.diagnostics().collection_count >= before + 5U;
    }));
    collector.stop();
    writer.stop();

    CHECK(collector.diagnostics().collection_count >= before + 5U);
    CHECK(collector.diagnostics().failed_samples == 0U);
    CHECK(writer.diagnostics().failed == 1U);
}

TEST_CASE("writer retries a transient archive failure without losing the incident",
          "[storage][writer][failure][recovery]") {
    core::IncidentCaptureCoordinator source{2U};
    source.start_accepting();
    RecoveringArchive archive;
    storage::IncidentWriter writer{source, archive, immediate_retries};
    writer.start();
    enqueue(source);
    REQUIRE(wait_until([&writer] { return writer.diagnostics().succeeded == 1U; }));

    const auto diagnostics = writer.diagnostics();
    CHECK(diagnostics.state == storage::WriterState::running);
    CHECK(diagnostics.attempts == 2U);
    CHECK(diagnostics.retry_attempts == 1U);
    CHECK(diagnostics.retry_exhausted == 0U);
    CHECK(diagnostics.failed == 0U);
    CHECK(diagnostics.recoveries == 1U);
    CHECK(diagnostics.consecutive_failures == 0U);
    CHECK(diagnostics.last_error_message.empty());
    writer.stop();
}

TEST_CASE("writer does not retry permanent archive failures",
          "[storage][writer][failure][permanent]") {
    core::IncidentCaptureCoordinator source{2U};
    source.start_accepting();
    FullArchive archive;
    storage::IncidentWriter writer{source, archive, immediate_retries};
    writer.start();
    enqueue(source);
    REQUIRE(wait_until([&writer] { return writer.diagnostics().failed == 1U; }));

    const auto diagnostics = writer.diagnostics();
    CHECK(archive.calls == 1U);
    CHECK(diagnostics.attempts == 1U);
    CHECK(diagnostics.retry_attempts == 0U);
    CHECK(diagnostics.retry_exhausted == 0U);
    CHECK(diagnostics.last_error_code == storage::StorageErrorCode::full);
    writer.stop();
}

TEST_CASE("writer retains one failed incident for explicit guided retry",
          "[storage][writer][failure][guided-recovery][bounded]") {
    core::IncidentCaptureCoordinator source{2U};
    source.start_accepting();
    ManuallyRecoveringArchive archive;
    storage::IncidentWriter writer{source, archive, immediate_retries};
    writer.start();
    enqueue(source);
    REQUIRE(wait_until([&writer] {
        return writer.diagnostics().recoverable_incident_available;
    }));
    CHECK(writer.recoverable_incident() != nullptr);
    CHECK_FALSE(writer.retry_recoverable());

    archive.available = true;
    const auto recovered = writer.retry_recoverable();
    REQUIRE(recovered.has_value());
    CHECK(*recovered == 77);
    CHECK(writer.recoverable_incident() == nullptr);
    CHECK_FALSE(writer.diagnostics().recoverable_incident_available);
    writer.stop();
}

TEST_CASE("writer rejects unbounded or inverted retry configurations",
          "[storage][writer][configuration]") {
    core::IncidentCaptureCoordinator source{2U};
    CountingArchive archive;
    CHECK_THROWS_AS(storage::IncidentWriter(
                        source, archive,
                        storage::IncidentWriterConfiguration{0U, 0ms, 0ms}),
                    std::invalid_argument);
    CHECK_THROWS_AS(storage::IncidentWriter(
                        source, archive,
                        storage::IncidentWriterConfiguration{3U, 10ms, 5ms}),
                    std::invalid_argument);
    CHECK_THROWS_AS(storage::IncidentWriter(
                        source, archive,
                        storage::IncidentWriterConfiguration{11U, 0ms, 0ms}),
                    std::invalid_argument);
}

TEST_CASE("repeated captures remain bounded and all accepted work drains",
          "[storage][writer][capture][soak]") {
    core::IncidentCaptureCoordinator source{2U};
    source.start_accepting();
    CountingArchive archive;
    storage::IncidentWriter writer{source, archive};
    writer.start();

    constexpr std::uint64_t captures = 100U;
    for (std::uint64_t index = 0U; index < captures; ++index) {
        enqueue(source);
        REQUIRE(wait_until([&writer, index] {
            return writer.diagnostics().succeeded == index + 1U;
        }));
        CHECK(source.status().queue_size <= source.status().queue_capacity);
    }
    writer.stop(storage::WriterStopPolicy::drain);

    CHECK(archive.calls.load() == captures);
    CHECK(writer.diagnostics().succeeded == captures);
    CHECK(writer.diagnostics().failed == 0U);
}

TEST_CASE("normal rolling collection submits no database work",
          "[storage][writer][no-continuous-writes]") {
    core::SystemMonotonicClock clock;
    mock::MockTelemetryProvider provider{clock};
    const auto configuration = telemetry::validate_recorder_configuration(
        telemetry::RecorderConfiguration{1ms, 10ms, 1ms});
    REQUIRE(configuration.has_value());
    telemetry::TelemetryCollector collector{provider, clock, *configuration};
    CountingArchive archive;
    storage::IncidentWriter writer{collector.incident_work_source(), archive};
    writer.start();
    collector.start();
    REQUIRE(wait_until([&collector] {
        return collector.diagnostics().collection_count >= 25U;
    }));
    collector.stop();
    writer.stop();

    CHECK(archive.calls.load() == 0U);
    CHECK(writer.diagnostics().attempts == 0U);
}

TEST_CASE("drain shutdown commits work already accepted by the bounded source",
          "[storage][writer][shutdown]") {
    core::IncidentCaptureCoordinator source{2U};
    source.start_accepting();
    CountingArchive archive;
    enqueue(source);

    storage::IncidentWriter writer{source, archive};
    writer.start();
    writer.stop(storage::WriterStopPolicy::drain);

    CHECK(archive.calls.load() == 1U);
    CHECK(writer.diagnostics().succeeded == 1U);
    CHECK(source.status().queue_size == 0U);
}

TEST_CASE("collector snapshot persists through the real asynchronous SQLite pipeline",
          "[storage][writer][sqlite][integration]") {
    core::SystemMonotonicClock clock;
    mock::MockTelemetryProvider provider{clock};
    auto values = telemetry::RecorderConfiguration{1ms, 20ms, 10ms};
    values.incident_pre_window = 2ms;
    values.incident_post_window = 2ms;
    const auto configuration = telemetry::validate_recorder_configuration(values);
    REQUIRE(configuration.has_value());
    telemetry::TelemetryCollector collector{provider, clock, *configuration};
    storage::SqliteIncidentArchive archive{{std::filesystem::path{":memory:"}}};
    REQUIRE(archive.open().has_value());
    storage::IncidentWriter writer{collector.incident_work_source(), archive};
    writer.start();
    collector.start();
    REQUIRE(wait_until([&collector] {
        return collector.diagnostics().collection_count >= 5U;
    }));
    REQUIRE(collector.request_incident_capture() ==
            core::IncidentCaptureRequestResult::started);
    REQUIRE(wait_until([&writer] { return writer.diagnostics().succeeded == 1U; }));
    collector.stop();
    writer.stop();

    const auto incidents = archive.list();
    REQUIRE(incidents.has_value());
    REQUIRE(incidents->size() == 1U);
    const auto loaded = archive.load(incidents->front().id);
    REQUIRE(loaded.has_value());
    CHECK_FALSE((*loaded)->system_samples().empty());
    CHECK(writer.diagnostics().failed == 0U);
}
