#include "core/clock.hpp"
#include "telemetry/collector.hpp"
#include "telemetry/mock/mock_telemetry_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <thread>

namespace core = blackbox::core;
namespace telemetry = blackbox::telemetry;
namespace mock = blackbox::telemetry::mock;
using namespace std::chrono_literals;

namespace {

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate,
                              const std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

[[nodiscard]] telemetry::ValidatedRecorderConfiguration configuration(
    const std::chrono::nanoseconds interval,
    const std::chrono::nanoseconds history) {
    auto result = telemetry::validate_recorder_configuration(
        telemetry::RecorderConfiguration{interval, history, 10ms});
    REQUIRE(result.has_value());
    return *result;
}

class ThrowingProvider final : public telemetry::ITelemetryProvider {
public:
    telemetry::ProviderSampleResult sample(
        telemetry::SamplingRequest,
        telemetry::RawTelemetrySnapshot&) override {
        throw std::runtime_error{"injected provider failure"};
    }

    [[nodiscard]] telemetry::PlatformCapabilities capabilities() const noexcept override {
        return {};
    }
};

class SlowProvider final : public telemetry::ITelemetryProvider {
public:
    [[nodiscard]] bool prepare_sampling_thread() noexcept override {
        ++prepare_calls;
        return true;
    }

    telemetry::ProviderSampleResult sample(
        telemetry::SamplingRequest,
        telemetry::RawTelemetrySnapshot& destination) override {
        entered.store(true);
        std::this_thread::sleep_for(20ms);
        destination.reset(std::chrono::steady_clock::now(),
                          telemetry::SamplingTierSet::all());
        return {};
    }

    [[nodiscard]] telemetry::PlatformCapabilities capabilities() const noexcept override {
        return {};
    }

    std::atomic<bool> entered{};
    std::atomic<std::uint64_t> prepare_calls{};
};

class ManualCadenceResetSignal final
    : public telemetry::ISamplingCadenceResetSignal {
public:
    [[nodiscard]] std::uint64_t cadence_reset_generation() const noexcept override {
        return generation.load();
    }
    void notify() noexcept { generation.fetch_add(1U); }
    std::atomic<std::uint64_t> generation{};
};

class TransitioningSlowProvider final : public telemetry::ITelemetryProvider {
public:
    TransitioningSlowProvider(const core::IMonotonicClock& value,
                              ManualCadenceResetSignal& reset_signal)
        : clock{value}, signal{reset_signal} {}

    telemetry::ProviderSampleResult sample(
        const telemetry::SamplingRequest request,
        telemetry::RawTelemetrySnapshot& destination) override {
        const auto sequence = samples.fetch_add(1U);
        if (sequence == 0U) {
            signal.notify();
            std::this_thread::sleep_for(30ms);
        }
        destination.reset(clock.now(), request.tiers);
        return {telemetry::ProviderSampleStatus::complete, sequence + 1U};
    }

    [[nodiscard]] telemetry::PlatformCapabilities capabilities() const noexcept override {
        return {};
    }

    const core::IMonotonicClock& clock;
    ManualCadenceResetSignal& signal;
    std::atomic<std::uint64_t> samples{};
};

class TierRecordingProvider final : public telemetry::ITelemetryProvider {
public:
    explicit TierRecordingProvider(const core::IMonotonicClock& clock)
        : mock_{clock} {}

    telemetry::ProviderSampleResult sample(
        const telemetry::SamplingRequest request,
        telemetry::RawTelemetrySnapshot& destination) override {
        ++samples;
        if (request.tiers.contains(telemetry::SamplingTier::slow)) {
            ++slow_samples;
        }
        return mock_.sample(request, destination);
    }

    [[nodiscard]] telemetry::PlatformCapabilities capabilities() const noexcept override {
        return mock_.capabilities();
    }

    mock::MockTelemetryProvider mock_;
    std::atomic<std::uint64_t> samples{};
    std::atomic<std::uint64_t> slow_samples{};
};

class OneShotDetector final : public telemetry::IAutomaticIncidentDetector {
public:
    std::optional<core::IncidentCaptureTrigger> observe(
        const telemetry::SystemSample&) noexcept override {
        ++state.samples_observed;
        if (emitted) return std::nullopt;
        emitted = true;
        ++state.triggers_emitted;
        return core::IncidentCaptureTrigger{
            core::IncidentTriggerKind::automatic,
            core::AutomaticIncidentResource::memory, 0.99, 0.50, 2.0};
    }
    void reset() noexcept override { emitted = false; }
    telemetry::AutomaticDetectorDiagnostics diagnostics() const noexcept override {
        return state;
    }
    bool emitted{};
    telemetry::AutomaticDetectorDiagnostics state{};
};

class LifecycleProvider final : public telemetry::ITelemetryProvider {
public:
    telemetry::ProviderSampleResult sample(
        const telemetry::SamplingRequest request,
        telemetry::RawTelemetrySnapshot& destination) override {
        const auto sequence = samples.fetch_add(1U) + 1U;
        destination.reset(clock.now(), request.tiers);
        destination.process_lifecycle_events.push_back({
            {{42U}, 84U}, telemetry::RawProcessLifecycleKind::started});
        return {telemetry::ProviderSampleStatus::partial, sequence};
    }
    [[nodiscard]] telemetry::PlatformCapabilities capabilities() const noexcept override {
        return {};
    }
    explicit LifecycleProvider(const core::IMonotonicClock& value) : clock{value} {}
    const core::IMonotonicClock& clock;
    std::atomic<std::uint64_t> samples{};
};

class LifecycleSink final : public telemetry::ISystemEventSink {
public:
    bool record_external_event(const core::SystemEvent& event) noexcept override {
        last = event;
        recorded.fetch_add(1U);
        return true;
    }
    std::atomic<std::uint64_t> recorded{};
    core::SystemEvent last{};
};

} // namespace

TEST_CASE("recorder configuration validates bounds and future cadences",
          "[telemetry][collector][configuration]") {
    const auto defaults = telemetry::validate_recorder_configuration({});
    REQUIRE(defaults.has_value());
    CHECK(defaults->capacity == 300U);
    CHECK(defaults->processes_per_frame_limit == 2'000U);
    CHECK(defaults->values.incident_pre_window == 120s);
    CHECK(defaults->values.incident_post_window == 30s);
    CHECK(defaults->values.resume_gap_threshold == 5s);

    const auto half_second = telemetry::validate_recorder_configuration(
        telemetry::RecorderConfiguration{500ms, 5min, 25ms});
    const auto quarter_second = telemetry::validate_recorder_configuration(
        telemetry::RecorderConfiguration{250ms, 5min, 12ms});
    REQUIRE(half_second.has_value());
    REQUIRE(quarter_second.has_value());
    CHECK(half_second->capacity == 600U);
    CHECK(quarter_second->capacity == 1'200U);
    CHECK(quarter_second->processes_per_frame_limit == 500U);

    CHECK(telemetry::validate_recorder_configuration(
              telemetry::RecorderConfiguration{0ns, 5min, 0ns}).error() ==
          telemetry::RecorderConfigurationError::interval_not_positive);
    CHECK(telemetry::validate_recorder_configuration(
              telemetry::RecorderConfiguration{1s, 0ns, 0ns}).error() ==
          telemetry::RecorderConfigurationError::history_not_positive);
    CHECK(telemetry::validate_recorder_configuration(
              telemetry::RecorderConfiguration{1s, 1min, -1ns}).error() ==
          telemetry::RecorderConfigurationError::late_tolerance_negative);
    CHECK(telemetry::validate_recorder_configuration(
              telemetry::RecorderConfiguration{1ns, 86'401ns, 0ns}).error() ==
          telemetry::RecorderConfigurationError::capacity_exceeded);
    auto invalid_metadata = telemetry::RecorderConfiguration{};
    invalid_metadata.metadata_interval = 0ns;
    CHECK(telemetry::validate_recorder_configuration(invalid_metadata).error() ==
          telemetry::RecorderConfigurationError::metadata_interval_not_positive);
    auto invalid_pre_window = telemetry::RecorderConfiguration{};
    invalid_pre_window.incident_pre_window = -1ns;
    CHECK(telemetry::validate_recorder_configuration(invalid_pre_window).error() ==
          telemetry::RecorderConfigurationError::incident_pre_window_negative);
    auto invalid_post_window = telemetry::RecorderConfiguration{};
    invalid_post_window.incident_post_window = -1ns;
    CHECK(telemetry::validate_recorder_configuration(invalid_post_window).error() ==
          telemetry::RecorderConfigurationError::incident_post_window_negative);
    auto invalid_resume_gap = telemetry::RecorderConfiguration{};
    invalid_resume_gap.resume_gap_threshold = 0ns;
    CHECK(telemetry::validate_recorder_configuration(invalid_resume_gap).error() ==
          telemetry::RecorderConfigurationError::resume_gap_threshold_not_positive);
}

TEST_CASE("monotonic schedule skips elapsed ticks without catch-up bursts",
          "[telemetry][collector][schedule]") {
    const auto scheduled = core::MonotonicTimePoint{10s};

    const auto on_time = telemetry::advance_schedule(scheduled, scheduled + 100ms, 1s);
    CHECK(on_time.next_deadline == scheduled + 1s);
    CHECK(on_time.deadline_overrun == 0ns);
    CHECK(on_time.dropped_ticks == 0U);
    CHECK_FALSE(on_time.deadline_missed);

    const auto exact_boundary = telemetry::advance_schedule(scheduled, scheduled + 1s, 1s);
    CHECK(exact_boundary.next_deadline == scheduled + 2s);
    CHECK(exact_boundary.deadline_overrun == 0ns);
    CHECK(exact_boundary.dropped_ticks == 1U);
    CHECK_FALSE(exact_boundary.deadline_missed);

    const auto stalled = telemetry::advance_schedule(scheduled, scheduled + 3200ms, 1s);
    CHECK(stalled.next_deadline == scheduled + 4s);
    CHECK(stalled.deadline_overrun == 2200ms);
    CHECK(stalled.dropped_ticks == 3U);
    CHECK(stalled.deadline_missed);
}

TEST_CASE("optional detector requests an automatic capture on the collector path",
          "[telemetry][collector][detector][integration]") {
    core::SystemMonotonicClock clock;
    mock::MockTelemetryProvider provider{clock};
    auto values = telemetry::RecorderConfiguration{1ms, 20ms, 1ms};
    values.incident_pre_window = 5ms;
    values.incident_post_window = 0ms;
    const auto validated = telemetry::validate_recorder_configuration(values);
    REQUIRE(validated.has_value());
    OneShotDetector detector;
    telemetry::TelemetryCollector collector{provider, clock, *validated, &detector};
    collector.start();
    REQUIRE(wait_until([&] {
        return collector.diagnostics().incident_capture.incidents_completed >= 1U;
    }));
    collector.stop();
    const auto incident = collector.try_dequeue_incident();
    REQUIRE(incident != nullptr);
    CHECK(incident->header().window.manual_trigger_count == 0U);
    CHECK(incident->header().window.automatic_trigger_count == 1U);
    CHECK(incident->header().window.automatic_resource ==
          core::AutomaticIncidentResource::memory);
    const auto diagnostics = collector.diagnostics();
    CHECK(diagnostics.automatic_detection_enabled);
    CHECK(diagnostics.automatic_captures_started == 1U);
}

TEST_CASE("process lifecycle is opt-in and suppresses the first inventory",
          "[telemetry][collector][process][privacy][warmup]") {
    core::SystemMonotonicClock clock;
    LifecycleProvider provider{clock};
    LifecycleSink sink;
    telemetry::TelemetryCollector collector{
        provider, clock, configuration(1ms, 10ms), nullptr, nullptr, &sink};
    collector.set_process_lifecycle_enabled(true);
    collector.start();
    REQUIRE(wait_until([&] { return provider.samples.load() >= 3U; }));
    collector.stop();

    CHECK(sink.recorded.load() >= 2U);
    CHECK(sink.recorded.load() < provider.samples.load());
    CHECK(sink.last.source == core::SystemEventSource::process);
    CHECK(sink.last.kind == core::SystemEventKind::process_started);
    CHECK(sink.last.process_pid == 42U);
    const auto diagnostics = collector.diagnostics();
    CHECK(diagnostics.process_lifecycle_observations >= 3U);
    CHECK(diagnostics.process_lifecycle_events_recorded == sink.recorded.load());
}

TEST_CASE("resume gaps are separated from ordinary scheduling jitter",
          "[telemetry][collector][resume]") {
    const auto scheduled = core::MonotonicTimePoint{10s};

    const auto ordinary = telemetry::detect_resume_gap(
        scheduled, scheduled + 4999ms, 1s, 5s);
    CHECK_FALSE(ordinary.detected);
    CHECK(ordinary.gap == 4999ms);
    CHECK(ordinary.skipped_ticks == 0U);

    const auto resumed = telemetry::detect_resume_gap(
        scheduled, scheduled + 12s, 1s, 5s);
    CHECK(resumed.detected);
    CHECK(resumed.gap == 12s);
    CHECK(resumed.skipped_ticks == 12U);

    CHECK_FALSE(telemetry::detect_resume_gap(
                    scheduled, scheduled + 12s, 0ns, 5s)
                    .detected);
}

TEST_CASE("power transition generation rebases an in-flight collection without drops",
          "[telemetry][collector][resume][schedule]") {
    core::SystemMonotonicClock clock;
    ManualCadenceResetSignal signal;
    TransitioningSlowProvider provider{clock, signal};
    telemetry::TelemetryCollector collector{
        provider, clock, configuration(5ms, 100ms), nullptr, nullptr, nullptr, &signal};

    collector.start();
    REQUIRE(wait_until([&] {
        return collector.diagnostics().collection_count >= 2U;
    }));
    collector.stop();

    const auto diagnostics = collector.diagnostics();
    CHECK(diagnostics.dropped_samples == 0U);
    CHECK(diagnostics.deadline_misses == 0U);
    CHECK(signal.cadence_reset_generation() == 1U);
}

TEST_CASE("ordinary in-flight stalls still fail the zero-drop schedule contract",
          "[telemetry][collector][schedule][stress]") {
    core::SystemMonotonicClock clock;
    ManualCadenceResetSignal signal;
    TransitioningSlowProvider provider{clock, signal};
    telemetry::TelemetryCollector collector{
        provider, clock, configuration(5ms, 100ms)};

    collector.start();
    REQUIRE(wait_until([&] {
        return collector.diagnostics().collection_count >= 2U;
    }));
    collector.stop();

    const auto diagnostics = collector.diagnostics();
    CHECK(diagnostics.dropped_samples >= 1U);
    CHECK(diagnostics.deadline_misses >= 1U);
}

TEST_CASE("pausing and restarting collection preserves history but resets rate baselines",
          "[telemetry][collector][pause][resume]") {
    core::SystemMonotonicClock clock;
    mock::MockTelemetryProvider provider{clock};
    telemetry::TelemetryCollector collector{provider, clock, configuration(100ms, 2s)};

    collector.start();
    REQUIRE(wait_until([&] { return collector.diagnostics().collection_count >= 3U; }));
    collector.stop();
    const auto before = collector.diagnostics();
    REQUIRE(before.ring.size >= 3U);

    collector.start();
    REQUIRE(wait_until([&] {
        return collector.diagnostics().collection_count >= before.collection_count + 1U;
    }));
    const auto first_after_resume = collector.snapshot(1U);
    REQUIRE(first_after_resume.size() == 1U);
    CHECK(first_after_resume.samples().back().cpu_usage.status ==
          telemetry::MetricStatus::temporarily_unavailable);
    CHECK(collector.diagnostics().ring.size >= before.ring.size);

    REQUIRE(wait_until([&] {
        return collector.diagnostics().collection_count >= before.collection_count + 2U;
    }));
    collector.stop();
    const auto second_after_resume = collector.snapshot(1U);
    REQUIRE(second_after_resume.size() == 1U);
    CHECK(second_after_resume.samples().back().cpu_usage.status ==
          telemetry::MetricStatus::available);
}

TEST_CASE("collector soaks through wraps while bounded readers request snapshots",
          "[telemetry][collector][soak][concurrency]") {
    core::SystemMonotonicClock clock;
    mock::MockTelemetryProvider provider{clock};
    telemetry::TelemetryCollector collector{provider, clock, configuration(1ms, 5ms)};
    std::atomic<bool> reading{true};
    std::atomic<bool> oversized{};

    collector.start();
    std::jthread reader{[&](std::stop_token stop) {
        while (reading.load() && !stop.stop_requested()) {
            if (collector.snapshot(3U).size() > 3U) {
                oversized.store(true);
            }
            // Keep the accelerated reader adversarial without monopolizing a Debug
            // scheduler quantum and turning this boundedness soak into a host-load test.
            std::this_thread::sleep_for(50us);
        }
    }};

    // Twenty-five complete wraps retain the boundedness/concurrency stress while
    // remaining independent of the coarser timer resolution on hosted Windows.
    constexpr std::uint64_t soak_samples = 128U;
    REQUIRE(wait_until(
        [&] { return collector.diagnostics().collection_count >= soak_samples; }, 5s));
    collector.stop();
    reading.store(false);
    reader.join();

    const auto diagnostics = collector.diagnostics();
    CHECK_FALSE(diagnostics.running);
    CHECK_FALSE(oversized.load());
    CHECK(diagnostics.ring.capacity == 5U);
    CHECK(diagnostics.ring.size == 5U);
    CHECK(diagnostics.ring.overwritten_samples >= soak_samples - 5U);
    CHECK(diagnostics.process_ring.capacity == 5U);
    CHECK(diagnostics.process_ring.size == 5U);
    CHECK(diagnostics.process_ring.overwritten_samples >= soak_samples - 5U);
    CHECK(diagnostics.active_processes == 1U);
    CHECK(diagnostics.process_metadata_entries == 1U);
    const auto active = collector.active_process_snapshot();
    REQUIRE(active.frame.processes.size() == 1U);
    REQUIRE(active.metadata.size() == 1U);
    CHECK(active.metadata.front().identity == active.frame.processes.front().identity);
    CHECK(diagnostics.failed_samples == 0U);
    CHECK(diagnostics.collection_count >= soak_samples);
    // Dropped ticks measure host scheduling pressure at this deliberately
    // accelerated 1 ms cadence and are not a boundedness invariant. A missed
    // deadline is counted at most once per completed collection, regardless
    // of how many elapsed ticks were skipped.
    CHECK(diagnostics.deadline_misses <= diagnostics.collection_count);
}

TEST_CASE("collector reconfiguration clears history and restarts its epoch",
          "[telemetry][collector][configuration]") {
    core::SystemMonotonicClock clock;
    mock::MockTelemetryProvider provider{clock};
    telemetry::TelemetryCollector collector{provider, clock, configuration(2ms, 10ms)};
    collector.start();
    REQUIRE(wait_until([&] { return collector.diagnostics().collection_count >= 5U; }));

    collector.reconfigure(configuration(1ms, 3ms));
    REQUIRE(wait_until([&] { return collector.diagnostics().collection_count >= 5U; }));
    collector.stop();

    const auto diagnostics = collector.diagnostics();
    CHECK(diagnostics.ring.epoch == 1U);
    CHECK(diagnostics.ring.capacity == 3U);
    CHECK(diagnostics.ring.size == 3U);
    CHECK(collector.snapshot(3U).epoch() == 1U);
}

TEST_CASE("provider exceptions become recorded unavailable samples",
          "[telemetry][collector][failure]") {
    core::SystemMonotonicClock clock;
    ThrowingProvider provider;
    telemetry::TelemetryCollector collector{provider, clock, configuration(1ms, 4ms)};
    collector.start();
    REQUIRE(wait_until([&] { return collector.diagnostics().failed_samples >= 3U; }));
    collector.stop();

    const auto snapshot = collector.snapshot(4U);
    REQUIRE_FALSE(snapshot.empty());
    CHECK(snapshot.samples().back().cpu_usage.status ==
          telemetry::MetricStatus::temporarily_unavailable);
    CHECK(collector.diagnostics().provider_status ==
          telemetry::ProviderSampleStatus::temporarily_failed);
}

TEST_CASE("cooperative shutdown joins a collection already in progress",
          "[telemetry][collector][shutdown]") {
    core::SystemMonotonicClock clock;
    SlowProvider provider;
    telemetry::TelemetryCollector collector{provider, clock, configuration(1ms, 4ms)};
    collector.start();
    REQUIRE(wait_until([&] { return provider.entered.load(); }));

    const auto started = std::chrono::steady_clock::now();
    collector.stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK_FALSE(collector.running());
    CHECK(elapsed < 1s);
    const auto diagnostics = collector.diagnostics();
    CHECK(diagnostics.collection_count == 1U);
    CHECK(diagnostics.sampling_thread_prepared);
    CHECK(provider.prepare_calls.load() == 1U);
    const auto scheduling = collector.scheduling_drop_snapshot();
    REQUIRE(scheduling.events.size() == 1U);
    CHECK(scheduling.overflow == 0U);
    CHECK(scheduling.events[0].collection_index == 1U);
    CHECK(scheduling.events[0].dropped_ticks >= 1U);
    CHECK(scheduling.events[0].deadline_overrun >= 19ms);
}

TEST_CASE("collector schedules slow metadata independently from normal counters",
          "[telemetry][collector][process][tiers]") {
    core::SystemMonotonicClock clock;
    TierRecordingProvider provider{clock};
    // Keep both cadences above coarse hosted-Windows timer resolution. The
    // contract is that normal samples occur between independently scheduled
    // slow metadata samples, not that a 1 ms timer produces a fixed count.
    auto values = telemetry::RecorderConfiguration{20ms, 1s, 10ms};
    values.metadata_interval = 250ms;
    const auto configured = telemetry::validate_recorder_configuration(values);
    REQUIRE(configured.has_value());
    telemetry::TelemetryCollector collector{provider, clock, *configured};
    collector.start();
    REQUIRE(wait_until([&] { return provider.slow_samples.load() >= 2U; }));
    collector.stop();

    CHECK(provider.slow_samples.load() >= 2U);
    CHECK(provider.slow_samples.load() < provider.samples.load());
    CHECK(collector.active_process_snapshot().metadata.size() == 1U);
}

TEST_CASE("collector completes capture after post-window without pausing sampling",
          "[telemetry][collector][incident]") {
    core::SystemMonotonicClock clock;
    mock::MockTelemetryProvider provider{clock};
    auto values = telemetry::RecorderConfiguration{1ms, 20ms, 10ms};
    values.incident_pre_window = 5ms;
    values.incident_post_window = 4ms;
    const auto configured = telemetry::validate_recorder_configuration(values);
    REQUIRE(configured.has_value());
    telemetry::TelemetryCollector collector{provider, clock, *configured};
    collector.start();
    REQUIRE(wait_until([&] { return collector.diagnostics().collection_count >= 8U; }));
    const auto before_capture = collector.diagnostics().collection_count;

    REQUIRE(collector.request_incident_capture() ==
            core::IncidentCaptureRequestResult::started);
    REQUIRE(wait_until([&] {
        return collector.incident_capture_status().incidents_completed == 1U;
    }));
    const auto after_capture = collector.diagnostics().collection_count;
    collector.stop();

    CHECK(after_capture > before_capture);
    const auto incident = collector.try_dequeue_incident();
    REQUIRE(incident != nullptr);
    CHECK_FALSE(incident->system_samples().empty());
    CHECK_FALSE(incident->process_samples().empty());
    CHECK(incident->header().actual_end >= incident->header().window.requested_end);
    CHECK(collector.diagnostics().incident_snapshot_timing.samples_recorded == 1U);
    CHECK(collector.request_incident_capture() ==
          core::IncidentCaptureRequestResult::stopped);
}
