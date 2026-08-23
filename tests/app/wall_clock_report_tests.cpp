#include "app/wall_clock_report.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace app = blackbox::app;

namespace {

[[nodiscard]] std::filesystem::path unique_directory() {
    return std::filesystem::temp_directory_path() /
           ("blackbox-wall-clock-report-" + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
}

[[nodiscard]] app::WallClockReport valid_report() {
    app::WallClockReport report{};
    report.application_version = "0.15.0";
    report.platform = "Windows";
    report.video_driver = "windows";
    report.source_revision = "local-uncommitted";
    report.completed = true;
    report.requested_runtime_seconds = 30U;
    report.capture_interval_seconds = 10U;
    report.collections = 30U;
    report.ring_capacity = 10U;
    report.ring_size = 10U;
    report.ring_total_appends = 30U;
    report.ring_overwritten_samples = 20U;
    report.incidents_completed = 2U;
    report.device_events_recorded = 1U;
    report.network_events_recorded = 2U;
    report.graphics_events_recorded = 3U;
    report.storage_events_recorded = 4U;
    report.writer_succeeded = 2U;
    report.archive_healthy = true;
    report.archive_incidents = 2U;
    report.archive_schema_version = 1;
    report.session_notifications_available = true;
    report.session_locks = 1U;
    report.session_unlocks = 1U;
    return report;
}

} // namespace

TEST_CASE("wall-clock report publishes a path-free direct-v1 artifact atomically",
          "[app][wall-clock][soak][direct-v1]") {
    const auto directory = unique_directory();
    std::filesystem::create_directories(directory);
    const auto destination = directory / "report.ini";

    const auto written = app::write_wall_clock_report(destination, valid_report());
    REQUIRE(written.has_value());
    CHECK(std::filesystem::is_regular_file(destination));
    CHECK_FALSE(std::filesystem::exists(destination.string() + ".partial"));

    std::ifstream input{destination, std::ios::binary};
    const std::string contents{std::istreambuf_iterator<char>{input}, {}};
    CHECK(contents.starts_with(
        "format=1\napplication_version=0.15.0\nplatform=Windows\n"
        "video_driver=windows\nsource_revision=local-uncommitted\n"));
    CHECK(contents.find("completed=1\n") != std::string::npos);
    CHECK(contents.find("collections=30\n") != std::string::npos);
    CHECK(contents.find("scheduling_drop_event_count=0\n") != std::string::npos);
    CHECK(contents.find("scheduling_drop_event_overflow=0\n") != std::string::npos);
    CHECK(contents.find("scheduling_drop_events=none\n") != std::string::npos);
    CHECK(contents.find("ring_overwritten_samples=20\n") != std::string::npos);
    CHECK(contents.find("writer_succeeded=2\n") != std::string::npos);
    CHECK(contents.find("window_visible=1\n") != std::string::npos);
    CHECK(contents.find("automatic_detection_enabled=0\n") != std::string::npos);
    CHECK(contents.find("automatic_detector_triggers=0\n") != std::string::npos);
    CHECK(contents.find("automatic_captures_started=0\n") != std::string::npos);
    CHECK(contents.find("automatic_event_requests=0\n") != std::string::npos);
    CHECK(contents.find("device_events_recorded=1\n") != std::string::npos);
    CHECK(contents.find("network_events_recorded=2\n") != std::string::npos);
    CHECK(contents.find("graphics_events_recorded=3\n") != std::string::npos);
    CHECK(contents.find("storage_events_recorded=4\n") != std::string::npos);
    CHECK(contents.find("session_notifications_available=1\n") !=
          std::string::npos);
    CHECK(contents.find("session_locks=1\nsession_unlocks=1\n") !=
          std::string::npos);
    CHECK(contents.find("archive_schema_version=1\n") != std::string::npos);
    CHECK(contents.find(directory.string()) == std::string::npos);
    input.close();

    const auto duplicate = app::write_wall_clock_report(destination, valid_report());
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().code == app::WallClockReportErrorCode::destination_exists);
    std::filesystem::remove_all(directory);
}

TEST_CASE("wall-clock report rejects unsafe identity duration and destination",
          "[app][wall-clock][soak][validation]") {
    auto report = valid_report();
    report.application_version = "unsafe\nvalue";
    const auto relative = app::write_wall_clock_report("report.ini", report);
    REQUIRE_FALSE(relative.has_value());
    CHECK(relative.error().code == app::WallClockReportErrorCode::invalid_report);

    const auto directory = unique_directory();
    std::filesystem::create_directories(directory);
    report = valid_report();
    report.requested_runtime_seconds = 7U * 24U * 60U * 60U + 1U;
    const auto unbounded = app::write_wall_clock_report(directory / "report.ini", report);
    REQUIRE_FALSE(unbounded.has_value());
    CHECK(unbounded.error().code == app::WallClockReportErrorCode::invalid_report);

    report = valid_report();
    report.scheduling_drop_events = {
        {10U, 1'700'000'000'000'000'000U, 200'000'000U, 1U},
        {15U, 1'700'000'005'000'000'000U, 1'200'000'000U, 2U}};
    const auto event_destination = directory / "events.ini";
    REQUIRE(app::write_wall_clock_report(event_destination, report).has_value());
    std::ifstream events{event_destination, std::ios::binary};
    const std::string event_contents{std::istreambuf_iterator<char>{events}, {}};
    CHECK(event_contents.find(
              "scheduling_drop_events=10:1700000000000000000:200000000:1;"
              "15:1700000005000000000:1200000000:2\n") != std::string::npos);
    events.close();

    report.scheduling_drop_events[1].collection_index = 9U;
    const auto unordered = app::write_wall_clock_report(
        directory / "unordered.ini", report);
    REQUIRE_FALSE(unordered.has_value());
    CHECK(unordered.error().code == app::WallClockReportErrorCode::invalid_report);
    std::filesystem::remove_all(directory);
}
