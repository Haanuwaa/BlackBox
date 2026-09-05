#include "core/clock.hpp"
#include "telemetry/event_collector.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

namespace core = blackbox::core;
namespace telemetry = blackbox::telemetry;
using namespace std::chrono_literals;

namespace {

class ScriptedEventProvider final : public telemetry::ISystemEventProvider {
public:
    telemetry::EventProviderStatus
    start(const telemetry::EventProviderConfiguration& configuration) noexcept override {
        last_configuration = configuration;
        ++starts;
        return telemetry::EventProviderStatus::complete;
    }

    telemetry::EventProviderPollResult
    poll(const core::MonotonicTimePoint observed_at,
         const std::span<core::SystemEvent> destination) noexcept override {
        const auto sequence = polls.fetch_add(1U);
        if (!destination.empty()) {
            destination[0].observed_at = observed_at;
            destination[0].source = emit_storage_retry       ? core::SystemEventSource::storage
                                    : emit_display_recovery  ? core::SystemEventSource::graphics
                                    : emit_application_crash ? core::SystemEventSource::application
                                    : emit_application_hang  ? core::SystemEventSource::application
                                                             : core::SystemEventSource::power;
            destination[0].kind =
                emit_storage_retry       ? core::SystemEventKind::storage_io_retry
                : emit_display_recovery  ? core::SystemEventKind::display_driver_recovery
                : emit_application_crash ? core::SystemEventKind::application_crash
                : emit_application_hang  ? core::SystemEventKind::application_hang
                : sequence % 2U == 0U    ? core::SystemEventKind::suspend
                                         : core::SystemEventKind::resume_automatic;
            destination[0].detail = sequence;
        }
        return {sequence == 0U ? telemetry::EventProviderStatus::temporarily_failed
                               : telemetry::EventProviderStatus::complete,
                destination.empty() ? 0U : 1U, 7U};
    }

    void stop() noexcept override { ++stops; }

    telemetry::EventProviderCapabilities capabilities() const noexcept override {
        return {.power_events = true, .device_events = true};
    }

    std::atomic<std::uint32_t> starts{};
    std::atomic<std::uint32_t> stops{};
    std::atomic<std::uint32_t> polls{};
    telemetry::EventProviderConfiguration last_configuration{};
    bool emit_application_crash{};
    bool emit_application_hang{};
    bool emit_display_recovery{};
    bool emit_storage_retry{};
};

class RecordingCaptureSink final : public core::IIncidentCaptureRequestSink {
public:
    core::IncidentCaptureRequestResult
    request_incident_capture(const core::MonotonicTimePoint event_time,
                             const core::IncidentCaptureTrigger trigger) noexcept override {
        last_event_time = event_time;
        last_trigger = trigger;
        ++requests;
        return result;
    }

    std::atomic<std::uint32_t> requests{};
    core::MonotonicTimePoint last_event_time{};
    core::IncidentCaptureTrigger last_trigger{};
    core::IncidentCaptureRequestResult result{core::IncidentCaptureRequestResult::started};
};

class SourceCyclingEventProvider final : public telemetry::ISystemEventProvider {
public:
    telemetry::EventProviderStatus
    start(const telemetry::EventProviderConfiguration&) noexcept override {
        return telemetry::EventProviderStatus::complete;
    }
    telemetry::EventProviderPollResult
    poll(const core::MonotonicTimePoint observed_at,
         const std::span<core::SystemEvent> destination) noexcept override {
        const auto sequence = polls.fetch_add(1U);
        if (destination.empty()) return {};
        destination[0].observed_at = observed_at;
        destination[0].source = static_cast<core::SystemEventSource>(sequence % 10U);
        return {telemetry::EventProviderStatus::complete, 1U, 0U};
    }
    void stop() noexcept override {}
    telemetry::EventProviderCapabilities capabilities() const noexcept override { return {}; }
    std::atomic<std::uint32_t> polls{};
};

template <typename Predicate> [[nodiscard]] bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

} // namespace

TEST_CASE("system event collector is independently bounded and reports recovery",
          "[telemetry][event-collector][bounded]") {
    core::SystemMonotonicClock clock;
    ScriptedEventProvider provider;
    telemetry::SystemEventCollector collector{
        provider, clock, {.poll_interval = 1ms, .ring_capacity = 3U}};

    collector.start();
    REQUIRE(wait_until([&] { return provider.polls.load() >= 6U; }));
    collector.stop();

    const auto events = collector.snapshot(100U);
    REQUIRE(events.size() == 3U);
    CHECK(events.samples()[0].detail + 1U == events.samples()[1].detail);
    CHECK(events.samples()[1].detail + 1U == events.samples()[2].detail);
    const auto diagnostics = collector.diagnostics();
    CHECK_FALSE(diagnostics.running);
    CHECK(diagnostics.ring.capacity == 3U);
    CHECK(diagnostics.ring.overwritten_samples >= 3U);
    CHECK(diagnostics.provider_failures == 1U);
    CHECK(diagnostics.provider_recoveries == 1U);
    CHECK(diagnostics.native_events_dropped == 7U);
    CHECK(collector.cadence_reset_generation() >= 6U);
    const auto cadence = collector.cadence_state();
    CHECK(cadence.native_resumes == cadence.generation / 2U);
    CHECK(cadence.last_resume_at != core::MonotonicTimePoint{});
    CHECK(provider.starts == 1U);
    CHECK(provider.stops == 1U);
}

TEST_CASE("system event collector can disable every source and reconfigure safely",
          "[telemetry][event-collector][privacy][configuration]") {
    core::SystemMonotonicClock clock;
    ScriptedEventProvider provider;
    telemetry::SystemEventCollector collector{
        provider, clock, {.poll_interval = 2ms, .ring_capacity = 4U}};
    collector.start();
    REQUIRE(wait_until([&] { return provider.polls.load() >= 2U; }));

    telemetry::EventCollectorConfiguration disabled{
        .poll_interval = 1ms,
        .ring_capacity = 2U,
        .provider = {false, false, false, false, false, false, false, false, false, false}};
    collector.reconfigure(disabled);
    REQUIRE(wait_until([&] { return provider.starts.load() >= 2U; }));
    collector.stop();

    CHECK(provider.last_configuration == disabled.provider);
    CHECK(collector.diagnostics().configuration == disabled);
    CHECK(collector.snapshot(10U).size() <= 2U);
    CHECK(provider.starts == 2U);
    CHECK(provider.stops == 2U);
}

TEST_CASE("system event diagnostics retain bounded counts for every event source",
          "[telemetry][event-collector][diagnostics][sources]") {
    core::SystemMonotonicClock clock;
    SourceCyclingEventProvider provider;
    telemetry::SystemEventCollector collector{
        provider, clock, {.poll_interval = 1ms, .ring_capacity = 20U}};
    collector.start();
    REQUIRE(wait_until([&] { return provider.polls.load() >= 20U; }));
    collector.stop();

    const auto counts = collector.diagnostics().events_by_source;
    CHECK(counts.power >= 2U);
    CHECK(counts.device >= 2U);
    CHECK(counts.audio >= 2U);
    CHECK(counts.service_manager >= 2U);
    CHECK(counts.security >= 2U);
    CHECK(counts.update >= 2U);
    CHECK(counts.application >= 2U);
    CHECK(counts.network >= 2U);
    CHECK(counts.graphics >= 2U);
    CHECK(counts.storage >= 2U);
    CHECK(counts.process == 0U);
    CHECK(counts.power + counts.device + counts.audio + counts.service_manager + counts.security +
              counts.update + counts.application + counts.network + counts.graphics +
              counts.storage + counts.process ==
          collector.diagnostics().events_recorded);
}

TEST_CASE("external process lifecycle evidence is strictly validated and bounded",
          "[telemetry][event-collector][process][privacy][bounded]") {
    core::SystemMonotonicClock clock;
    ScriptedEventProvider provider;
    telemetry::SystemEventCollector collector{
        provider, clock, {.poll_interval = 1s, .ring_capacity = 2U}};

    core::SystemEvent event{};
    event.observed_at = clock.now();
    event.source = core::SystemEventSource::process;
    event.kind = core::SystemEventKind::process_started;
    event.has_process_identity = true;
    event.process_pid = 42U;
    event.process_creation_token = 84U;
    CHECK(collector.record_external_event(event));
    event.kind = core::SystemEventKind::process_exited;
    CHECK(collector.record_external_event(event));
    event.kind = core::SystemEventKind::process_started;
    CHECK(collector.record_external_event(event));
    CHECK(collector.snapshot(10U).size() == 2U);

    event.has_process_identity = false;
    CHECK_FALSE(collector.record_external_event(event));
    event.has_process_identity = true;
    event.source = core::SystemEventSource::application;
    CHECK_FALSE(collector.record_external_event(event));

    const auto diagnostics = collector.diagnostics();
    CHECK(diagnostics.events_recorded == 3U);
    CHECK(diagnostics.external_events_recorded == 3U);
    CHECK(diagnostics.events_by_source.process == 3U);
    CHECK(diagnostics.ring.overwritten_samples == 1U);
}

TEST_CASE("measured storage retry events request bounded automatic capture",
          "[telemetry][event-collector][automatic][storage]") {
    core::SystemMonotonicClock clock;
    ScriptedEventProvider provider;
    provider.emit_storage_retry = true;
    RecordingCaptureSink sink;
    telemetry::SystemEventCollector collector{
        provider, clock, {.poll_interval = 1ms, .ring_capacity = 8U}};
    collector.set_incident_capture_sink(&sink);

    collector.start();
    REQUIRE(wait_until([&] { return sink.requests.load() >= 1U; }));
    collector.stop();

    CHECK(sink.last_trigger.kind == core::IncidentTriggerKind::automatic);
    CHECK(sink.last_trigger.resource == core::AutomaticIncidentResource::disk);
    CHECK(sink.last_trigger.signal == core::AutomaticIncidentSignal::storage_io_retry);
    CHECK(sink.last_trigger.score == 1.0);
    CHECK(collector.cadence_reset_generation() == 0U);
    CHECK(collector.diagnostics().automatic_event_requests >= 1U);
    CHECK(collector.diagnostics().automatic_event_captures_started >= 1U);
}

TEST_CASE("system event collector rejects unbounded or nonpositive configuration",
          "[telemetry][event-collector][validation]") {
    core::SystemMonotonicClock clock;
    ScriptedEventProvider provider;
    CHECK_THROWS(telemetry::SystemEventCollector(provider, clock,
                                                 {.poll_interval = 0ms, .ring_capacity = 1U}));
    CHECK_THROWS(telemetry::SystemEventCollector(
        provider, clock,
        {.poll_interval = 1ms, .ring_capacity = telemetry::maximum_event_ring_capacity + 1U}));
}

TEST_CASE("measured Windows application crash events request bounded automatic "
          "capture",
          "[telemetry][event-collector][automatic][crash]") {
    core::SystemMonotonicClock clock;
    ScriptedEventProvider provider;
    provider.emit_application_crash = true;
    RecordingCaptureSink sink;
    telemetry::SystemEventCollector collector{
        provider, clock, {.poll_interval = 1ms, .ring_capacity = 8U}};
    collector.set_incident_capture_sink(&sink);

    collector.start();
    REQUIRE(wait_until([&] { return sink.requests.load() >= 1U; }));
    collector.stop();

    CHECK(sink.last_trigger.kind == core::IncidentTriggerKind::automatic);
    CHECK(sink.last_trigger.resource == core::AutomaticIncidentResource::none);
    CHECK(sink.last_trigger.signal == core::AutomaticIncidentSignal::application_crash);
    CHECK(sink.last_trigger.score == 1.0);
    CHECK(collector.diagnostics().automatic_event_requests >= 1U);
    CHECK(collector.diagnostics().automatic_event_captures_started >= 1U);
}

TEST_CASE("measured Windows application hang events request bounded automatic "
          "capture",
          "[telemetry][event-collector][automatic][hang]") {
    core::SystemMonotonicClock clock;
    ScriptedEventProvider provider;
    provider.emit_application_hang = true;
    RecordingCaptureSink sink;
    telemetry::SystemEventCollector collector{
        provider, clock, {.poll_interval = 1ms, .ring_capacity = 8U}};
    collector.set_incident_capture_sink(&sink);

    collector.start();
    REQUIRE(wait_until([&] { return sink.requests.load() >= 1U; }));
    collector.stop();

    CHECK(sink.last_trigger.kind == core::IncidentTriggerKind::automatic);
    CHECK(sink.last_trigger.resource == core::AutomaticIncidentResource::none);
    CHECK(sink.last_trigger.signal == core::AutomaticIncidentSignal::application_hang);
    CHECK(sink.last_trigger.score == 1.0);
    CHECK(collector.diagnostics().automatic_event_requests >= 1U);
    CHECK(collector.diagnostics().automatic_event_captures_started >= 1U);
}

TEST_CASE("measured display recovery events request bounded automatic capture",
          "[telemetry][event-collector][automatic][graphics]") {
    core::SystemMonotonicClock clock;
    ScriptedEventProvider provider;
    provider.emit_display_recovery = true;
    RecordingCaptureSink sink;
    telemetry::SystemEventCollector collector{
        provider, clock, {.poll_interval = 1ms, .ring_capacity = 8U}};
    collector.set_incident_capture_sink(&sink);

    collector.start();
    REQUIRE(wait_until([&] { return sink.requests.load() >= 1U; }));
    collector.stop();

    CHECK(sink.last_trigger.kind == core::IncidentTriggerKind::automatic);
    CHECK(sink.last_trigger.resource == core::AutomaticIncidentResource::none);
    CHECK(sink.last_trigger.signal == core::AutomaticIncidentSignal::display_driver_recovery);
    CHECK(sink.last_trigger.score == 1.0);
    CHECK(collector.diagnostics().automatic_event_requests >= 1U);
    CHECK(collector.diagnostics().automatic_event_captures_started >= 1U);
}
