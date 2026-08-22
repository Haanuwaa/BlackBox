#include "storage/test_incident.hpp"
#include "ui/incident_viewer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace core = blackbox::core;
namespace storage = blackbox::storage;
namespace ui = blackbox::ui;
using namespace std::chrono_literals;

TEST_CASE("incident view model preserves ranges marker-relative time and missing states",
          "[ui][viewer][view-model]") {
    const auto incident = storage::test::representative_incident();
    const auto detail = ui::build_incident_detail(
        42, 0, "Network", "packet loss", *incident, incident->process_samples()[0].identity);

    CHECK(detail.id == 42);
    CHECK(detail.created_utc == "1970-01-01 00:00:00.000 UTC");
    CHECK(detail.requested_start_seconds == -120.0);
    CHECK(detail.requested_end_seconds == 30.0);
    CHECK(detail.actual_start_seconds == -95.0);
    CHECK(detail.actual_end_seconds == 30.0);
    CHECK(detail.manual_trigger_count == 1U);
    CHECK(detail.automatic_trigger_count == 1U);
    CHECK(detail.automatic_resource == core::AutomaticIncidentResource::cpu);
    CHECK(detail.automatic_signal == core::AutomaticIncidentSignal::disk_latency);
    CHECK(detail.automatic_observed_value == 0.99);
    REQUIRE(detail.cpu_percent.values.size() == 2U);
    CHECK(detail.cpu_percent.seconds_from_event.front() == -95.0);
    CHECK(detail.cpu_percent.seconds_from_event.back() == 30.0);
    CHECK(detail.network_receive_mib_per_second.values.empty());
    CHECK(detail.network_receive_mib_per_second.availability.by_status[1] == 2U);
    REQUIRE(detail.disk_read_latency_milliseconds.values.size() == 2U);
    CHECK(detail.disk_read_latency_milliseconds.values.front() == Catch::Approx{12.0});
    REQUIRE(detail.disk_service_time_milliseconds.values.size() == 2U);
    CHECK(detail.disk_service_time_milliseconds.values.front() == Catch::Approx{18.0});
    REQUIRE(detail.disk_queue_depth.values.size() == 2U);
    CHECK(detail.disk_queue_depth.values.front() == 3.5);
    REQUIRE(detail.network_connectivity_level.values.size() == 2U);
    CHECK(detail.network_connectivity_level.values.front() == 2.0);
    REQUIRE(detail.network_tcp_retransmit_percent.values.size() == 2U);
    CHECK(detail.network_tcp_retransmit_percent.values.front() == Catch::Approx{12.5});
    REQUIRE(detail.gpu_percent.values.size() == 2U);
    CHECK(detail.gpu_percent.values.front() == Catch::Approx{80.0});
    CHECK(detail.gpu_dedicated_memory_mib.values.front() == Catch::Approx{3'072.0});
    CHECK(detail.foreground_gpu_percent.values.front() == Catch::Approx{70.0});
    CHECK(detail.dpc_percent.values.front() == Catch::Approx{3.0});
    CHECK(detail.interrupt_percent.values.front() == Catch::Approx{2.0});
    CHECK(detail.cpu_current_mhz.values.front() == Catch::Approx{3'200.0});
    CHECK(detail.cpu_thermal_limit_percent.values.front() == Catch::Approx{80.0});
    CHECK(detail.battery_percent.values.front() == Catch::Approx{42.0});
    REQUIRE(detail.foreground_applications.size() == 1U);
    CHECK(detail.foreground_applications.front().name == "PID 99");
    CHECK(detail.foreground_applications.front().gpu_percent == Catch::Approx{70.0});
    REQUIRE(detail.system_events.size() == 1U);
    CHECK(detail.system_events.front().source == "Network");
    CHECK(detail.system_events.front().event == "DNS resolution timeout reported");
    CHECK(detail.system_events.front().native_event_id == 1014U);
    REQUIRE(detail.processes.size() == 1U);
    CHECK(detail.processes.front().identity == incident->process_samples()[0].identity);
    REQUIRE(detail.selected_process_cpu_percent.values.size() == 1U);
    CHECK(detail.selected_process_cpu_percent.values.front() == 12.5);
}

TEST_CASE("display timeout recovery has a privacy-normalized view model",
          "[ui][viewer][view-model][windows-events]") {
    const auto fixture = storage::test::representative_incident();
    auto header = fixture->header();
    header.window.automatic_resource = core::AutomaticIncidentResource::none;
    header.window.automatic_signal = core::AutomaticIncidentSignal::display_driver_recovery;

    core::SystemEvent recovery{};
    recovery.observed_at = header.window.event_time;
    recovery.source = core::SystemEventSource::graphics;
    recovery.kind = core::SystemEventKind::display_driver_recovery;
    recovery.level = core::SystemEventLevel::warning;
    recovery.native_event_id = 4101U;

    const core::IncidentSnapshot incident{
        std::move(header),
        std::vector<core::IncidentSystemSample>{fixture->system_samples().begin(),
                                                fixture->system_samples().end()},
        std::vector<core::IncidentProcessInfo>{fixture->process_metadata().begin(),
                                               fixture->process_metadata().end()},
        std::vector<core::IncidentProcessSample>{fixture->process_samples().begin(),
                                                 fixture->process_samples().end()},
        std::vector{recovery}};
    const auto detail = ui::build_incident_detail(43, 0, {}, {}, incident, std::nullopt);

    REQUIRE(detail.system_events.size() == 1U);
    CHECK(detail.system_events.front().source == "Graphics");
    CHECK(detail.system_events.front().event == "Display timeout recovery reported");
    CHECK(detail.system_events.front().native_event_id == 4101U);
}

TEST_CASE("application crash has a privacy-normalized view model",
          "[ui][viewer][view-model][windows-events][crash]") {
    const auto fixture = storage::test::representative_incident();
    auto header = fixture->header();
    header.window.automatic_resource = core::AutomaticIncidentResource::none;
    header.window.automatic_signal = core::AutomaticIncidentSignal::application_crash;

    core::SystemEvent crash{};
    crash.observed_at = header.window.event_time;
    crash.source = core::SystemEventSource::application;
    crash.kind = core::SystemEventKind::application_crash;
    crash.level = core::SystemEventLevel::error;
    crash.native_event_id = 1000U;

    const core::IncidentSnapshot incident{
        std::move(header),
        std::vector<core::IncidentSystemSample>{fixture->system_samples().begin(),
                                                fixture->system_samples().end()},
        std::vector<core::IncidentProcessInfo>{fixture->process_metadata().begin(),
                                               fixture->process_metadata().end()},
        std::vector<core::IncidentProcessSample>{fixture->process_samples().begin(),
                                                 fixture->process_samples().end()},
        std::vector{crash}};
    const auto detail = ui::build_incident_detail(45, 0, {}, {}, incident, std::nullopt);

    REQUIRE(detail.system_events.size() == 1U);
    CHECK(detail.system_events.front().source == "Application");
    CHECK(detail.system_events.front().event == "Application crash reported");
    CHECK(detail.system_events.front().level == "Error");
    CHECK(detail.system_events.front().native_event_id == 1000U);
}

TEST_CASE("storage retry has a privacy-normalized view model",
          "[ui][viewer][view-model][windows-events][storage]") {
    const auto fixture = storage::test::representative_incident();
    auto header = fixture->header();
    header.window.automatic_resource = core::AutomaticIncidentResource::disk;
    header.window.automatic_signal = core::AutomaticIncidentSignal::storage_io_retry;

    core::SystemEvent retry{};
    retry.observed_at = header.window.event_time;
    retry.source = core::SystemEventSource::storage;
    retry.kind = core::SystemEventKind::storage_io_retry;
    retry.level = core::SystemEventLevel::warning;
    retry.native_event_id = 153U;

    const core::IncidentSnapshot incident{
        std::move(header),
        std::vector<core::IncidentSystemSample>{fixture->system_samples().begin(),
                                                fixture->system_samples().end()},
        std::vector<core::IncidentProcessInfo>{fixture->process_metadata().begin(),
                                               fixture->process_metadata().end()},
        std::vector<core::IncidentProcessSample>{fixture->process_samples().begin(),
                                                 fixture->process_samples().end()},
        std::vector{retry}};
    const auto detail = ui::build_incident_detail(44, 0, {}, {}, incident, std::nullopt);

    REQUIRE(detail.system_events.size() == 1U);
    CHECK(detail.system_events.front().source == "Storage");
    CHECK(detail.system_events.front().event == "Storage I/O retry reported");
    CHECK(detail.system_events.front().native_event_id == 153U);
}

TEST_CASE("process lifecycle context resolves durable identity to a local name",
          "[ui][viewer][view-model][process][privacy]") {
    const auto fixture = storage::test::representative_incident();
    core::SystemEvent started{};
    started.observed_at = fixture->header().window.event_time;
    started.source = core::SystemEventSource::process;
    started.kind = core::SystemEventKind::process_started;
    started.has_process_identity = true;
    started.process_pid = fixture->process_metadata().front().identity.pid;
    started.process_creation_token =
        fixture->process_metadata().front().identity.creation_token;
    const core::IncidentSnapshot incident{
        fixture->header(),
        std::vector<core::IncidentSystemSample>{fixture->system_samples().begin(),
                                                fixture->system_samples().end()},
        std::vector<core::IncidentProcessInfo>{fixture->process_metadata().begin(),
                                               fixture->process_metadata().end()},
        std::vector<core::IncidentProcessSample>{fixture->process_samples().begin(),
                                                 fixture->process_samples().end()},
        std::vector{started}};

    const auto detail = ui::build_incident_detail(
        45, 0, {}, {}, incident, std::nullopt);
    REQUIRE(detail.system_events.size() == 1U);
    CHECK(detail.system_events.front().source == "Process");
    CHECK(detail.system_events.front().event == "Process started: fixture.exe");
}

TEST_CASE("min max timeline decimation retains a narrow spike within its point budget",
          "[ui][viewer][downsample]") {
    const auto incident = storage::test::scaled_incident(1U, 10'000U);
    const auto detail = ui::build_incident_detail(1, 0, {}, {}, *incident, std::nullopt, 100U);
    CHECK(detail.cpu_percent.values.size() <= 100U);
    CHECK(detail.cpu_percent.seconds_from_event.front() == -5'000.0);
    CHECK(detail.cpu_percent.seconds_from_event.back() == 4'999.0);
    REQUIRE(std::find(detail.cpu_percent.values.begin(), detail.cpu_percent.values.end(),
                      100.0) != detail.cpu_percent.values.end());
}

TEST_CASE("process filtering and sorting handles names paths PIDs and unavailable metrics",
          "[ui][viewer][filter][sort]") {
    std::vector<ui::IncidentProcessRow> rows(3U);
    rows[0].identity.pid = 30U;
    rows[0].name = "compiler.exe";
    rows[0].executable_path = "C:\\Tools\\Compiler.exe";
    rows[0].cpu_available = true;
    rows[0].peak_cpu_percent = 80.0;
    rows[1].identity.pid = 10U;
    rows[1].name = "game.exe";
    rows[1].cpu_available = true;
    rows[1].peak_cpu_percent = 10.0;
    rows[2].identity.pid = 20U;
    rows[2].name = "protected";

    auto filtered = ui::filter_and_sort_processes(
        rows, "TOOLS", ui::IncidentProcessSort::name, true);
    REQUIRE(filtered.size() == 1U);
    CHECK(filtered.front() == 0U);
    filtered = ui::filter_and_sort_processes(
        rows, "20", ui::IncidentProcessSort::pid, true);
    REQUIRE(filtered.size() == 1U);
    CHECK(filtered.front() == 2U);
    const auto cpu_descending = ui::filter_and_sort_processes(
        rows, {}, ui::IncidentProcessSort::peak_cpu, false);
    REQUIRE(cpu_descending.size() == 3U);
    CHECK(cpu_descending[0] == 0U);
    CHECK(cpu_descending[1] == 1U);
    CHECK(cpu_descending[2] == 2U);
}

TEST_CASE("incident editor synchronizes recurring override with immutable viewer content",
          "[ui][viewer][recurring][override]") {
    ui::IncidentViewerState state{};
    auto content = std::make_shared<ui::IncidentViewerContent>();
    content->generation = 1U;
    content->detail = ui::IncidentDetailView{};
    content->detail->id = 9;
    content->detail->recurring_group_override = "audio-stutter";
    state.content = content;
    ui::synchronize_incident_editor(state);
    CHECK(std::string{state.recurring_group_override_editor.data()} == "audio-stutter");

    content = std::make_shared<ui::IncidentViewerContent>(*content);
    content->generation = 2U;
    content->detail.reset();
    state.content = content;
    ui::synchronize_incident_editor(state);
    CHECK(state.recurring_group_override_editor.front() == '\0');
}
