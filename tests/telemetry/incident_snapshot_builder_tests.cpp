#include "telemetry/incident_snapshot_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

namespace core = blackbox::core;
namespace telemetry = blackbox::telemetry;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] telemetry::SystemSample system_sample(const std::chrono::seconds at) {
    telemetry::SystemSample sample{};
    sample.observed_at = core::MonotonicTimePoint{at};
    sample.cpu_usage = telemetry::MetricValue<telemetry::Ratio>::available({0.5});
    sample.memory_used = telemetry::MetricValue<telemetry::ByteCount>::available({42U});
    sample.cpu_some_pressure = telemetry::MetricValue<telemetry::Ratio>::available({0.25});
    sample.thermal_pressure_state =
        telemetry::MetricValue<telemetry::ThermalPressureState>::available(
            telemetry::ThermalPressureState::serious);
    sample.memory_pressure_state =
        telemetry::MetricValue<telemetry::MemoryPressureState>::available(
            telemetry::MemoryPressureState::warning);
    sample.foreground_application =
        telemetry::MetricValue<telemetry::OpaqueApplicationIdentity>::available({11U, 22U});
    return sample;
}

[[nodiscard]] telemetry::ProcessFrame process_frame(const std::chrono::seconds at,
                                                    const telemetry::ProcessIdentity identity) {
    telemetry::ProcessSample sample{};
    sample.identity = identity;
    sample.working_set = telemetry::MetricValue<telemetry::ByteCount>::available({84U});
    return telemetry::ProcessFrame{core::MonotonicTimePoint{at}, {sample}};
}

[[nodiscard]] telemetry::ProcessInfo process_info(const telemetry::ProcessIdentity identity,
                                                  const char* name) {
    telemetry::ProcessInfo info{};
    info.identity = identity;
    info.name = telemetry::MetricValue<std::string>::available(name);
    return info;
}

} // namespace

TEST_CASE("incident snapshot clips history and retains only referenced metadata",
          "[telemetry][incident][clock]") {
    const telemetry::ProcessIdentity referenced{{7U}, 70U};
    const telemetry::ProcessIdentity unused{{8U}, 80U};
    core::RecorderSnapshot<telemetry::SystemSample> systems{
        3U, {system_sample(0s), system_sample(10s), system_sample(20s), system_sample(30s)}};
    core::RecorderSnapshot<telemetry::ProcessFrame> processes{
        3U,
        {process_frame(0s, unused), process_frame(10s, referenced), process_frame(20s, referenced),
         process_frame(30s, referenced)}};
    const std::vector metadata{process_info(unused, "unused"),
                               process_info(referenced, "referenced")};
    const core::IncidentCaptureWindow window{9U, core::MonotonicTimePoint{20s},
                                             core::MonotonicTimePoint{5s},
                                             core::MonotonicTimePoint{30s}, 1U};

    const auto incident = telemetry::build_incident_snapshot(window, core::MonotonicTimePoint{30s},
                                                             systems, processes, metadata);
    REQUIRE(incident != nullptr);
    CHECK(incident->header().system_recorder_epoch == 3U);
    CHECK(incident->header().process_recorder_epoch == 3U);
    CHECK(incident->header().actual_start == core::MonotonicTimePoint{10s});
    CHECK(incident->header().actual_end == core::MonotonicTimePoint{30s});
    REQUIRE(incident->system_samples().size() == 3U);
    REQUIRE(incident->process_samples().size() == 3U);
    REQUIRE(incident->process_metadata().size() == 1U);
    CHECK(incident->process_metadata().front().identity.pid == 7U);
    CHECK(incident->process_metadata().front().name.value == "referenced");
    CHECK(incident->system_samples().front().cpu_fraction.value == 0.5);
    CHECK(incident->system_samples().front().cpu_some_pressure_fraction.value == 0.25);
    CHECK(incident->system_samples().front().thermal_pressure_state.value == 2U);
    CHECK(incident->system_samples().front().memory_pressure_state.value == 1U);
    CHECK(incident->system_samples().front().foreground_application.value.session_token == 11U);
    CHECK(incident->system_samples().front().foreground_application.value.application_token ==
          22U);
    CHECK(incident->process_samples().front().working_set_bytes.value == 84U);
}

TEST_CASE("incident snapshot short uptime starts at the oldest available sample",
          "[telemetry][incident][boundary]") {
    core::RecorderSnapshot<telemetry::SystemSample> systems{
        0U, {system_sample(50s), system_sample(60s)}};
    core::RecorderSnapshot<telemetry::ProcessFrame> processes{0U, {}};
    const core::IncidentCaptureWindow window{1U, core::MonotonicTimePoint{55s},
                                             core::MonotonicTimePoint{-65s},
                                             core::MonotonicTimePoint{60s}, 1U};

    const auto incident = telemetry::build_incident_snapshot(window, core::MonotonicTimePoint{60s},
                                                             systems, processes, {});
    REQUIRE(incident->system_samples().size() == 2U);
    CHECK(incident->header().actual_start == core::MonotonicTimePoint{50s});
    CHECK(incident->header().actual_end == core::MonotonicTimePoint{60s});
}

TEST_CASE("incident snapshot joins only in-window events into immutable evidence",
          "[telemetry][incident][events][privacy]") {
    core::RecorderSnapshot<telemetry::SystemSample> systems{
        1U, {system_sample(10s), system_sample(20s)}};
    core::RecorderSnapshot<telemetry::ProcessFrame> processes{2U, {}};
    const telemetry::ProcessIdentity lifecycle_identity{{77U}, 770U};
    core::RecorderSnapshot<core::SystemEvent> events{
        9U,
        {
            {.observed_at = core::MonotonicTimePoint{4s},
             .source = core::SystemEventSource::device,
             .kind = core::SystemEventKind::device_removed},
            {.observed_at = core::MonotonicTimePoint{10s},
             .source = core::SystemEventSource::audio,
             .kind = core::SystemEventKind::audio_default_changed},
            {.observed_at = core::MonotonicTimePoint{15s},
             .source = core::SystemEventSource::process,
             .kind = core::SystemEventKind::process_started,
             .has_process_identity = true,
             .process_pid = lifecycle_identity.pid.value,
             .process_creation_token = lifecycle_identity.creation_token},
            {.observed_at = core::MonotonicTimePoint{20s},
             .source = core::SystemEventSource::application,
             .kind = core::SystemEventKind::application_hang,
             .native_event_id = 1002U},
            {.observed_at = core::MonotonicTimePoint{31s},
             .source = core::SystemEventSource::power,
             .kind = core::SystemEventKind::suspend},
        }};
    const core::IncidentCaptureWindow window{11U, core::MonotonicTimePoint{20s},
                                             core::MonotonicTimePoint{5s},
                                             core::MonotonicTimePoint{30s}, 1U};

    const auto incident = telemetry::build_incident_snapshot(
        window, core::MonotonicTimePoint{30s}, systems, processes,
        std::vector{process_info(lifecycle_identity, "short-lived.exe")}, &events);
    REQUIRE(incident != nullptr);
    CHECK(incident->header().event_recorder_epoch == 9U);
    REQUIRE(incident->system_events().size() == 3U);
    CHECK(incident->system_events()[0].kind == core::SystemEventKind::audio_default_changed);
    CHECK(incident->system_events()[1].process_pid == 77U);
    CHECK(incident->system_events()[2].native_event_id == 1002U);
    REQUIRE(incident->process_metadata().size() == 1U);
    CHECK(incident->process_metadata().front().name.value == "short-lived.exe");
}
