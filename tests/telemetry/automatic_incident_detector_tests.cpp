#include "telemetry/automatic_incident_detector.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

namespace core = blackbox::core;
namespace telemetry = blackbox::telemetry;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] telemetry::SystemSample sample(const std::int64_t second,
                                             const double cpu,
                                             const double memory,
                                             const double disk,
                                             const double network) {
    telemetry::SystemSample result{};
    result.observed_at = core::MonotonicTimePoint{std::chrono::seconds{second}};
    result.cpu_usage = telemetry::MetricValue<telemetry::Ratio>::available({cpu});
    result.memory_usage = telemetry::MetricValue<telemetry::Ratio>::available({memory});
    result.disk_read_rate =
        telemetry::MetricValue<telemetry::BytesPerSecond>::available({disk});
    result.disk_write_rate =
        telemetry::MetricValue<telemetry::BytesPerSecond>::available({0.0});
    result.network_receive_rate =
        telemetry::MetricValue<telemetry::BytesPerSecond>::available({network});
    result.network_transmit_rate =
        telemetry::MetricValue<telemetry::BytesPerSecond>::available({0.0});
    return result;
}

[[nodiscard]] telemetry::AutomaticDetectorConfiguration test_configuration() {
    telemetry::AutomaticDetectorConfiguration configuration{};
    configuration.cpu = {0.90, 0.70, 0.01};
    configuration.memory = {0.95, 0.85, 0.01};
    configuration.disk = {100.0, 60.0, 5.0};
    configuration.network = {100.0, 60.0, 5.0};
    configuration.baseline_samples = 10U;
    configuration.consecutive_samples = 3U;
    configuration.statistical_z_score = 6.0;
    configuration.cooldown = 120s;
    return configuration;
}

[[nodiscard]] std::shared_ptr<const core::IncidentSnapshot> empty_incident(
    const core::IncidentCaptureWindow& window) {
    core::IncidentHeader header{};
    header.window = window;
    return std::make_shared<const core::IncidentSnapshot>(
        header, std::vector<core::IncidentSystemSample>{},
        std::vector<core::IncidentProcessInfo>{},
        std::vector<core::IncidentProcessSample>{});
}

} // namespace

TEST_CASE("automatic detector rejects unsafe configurations",
          "[telemetry][detector][configuration]") {
    auto configuration = test_configuration();
    configuration.baseline_samples = 61U;
    CHECK_FALSE(telemetry::validate_automatic_detector_configuration(configuration));
    configuration = test_configuration();
    configuration.cooldown = -1ns;
    CHECK_FALSE(telemetry::validate_automatic_detector_configuration(configuration));
    configuration = test_configuration();
    configuration.cpu.statistical_floor = configuration.cpu.hard_threshold + 1.0;
    CHECK_FALSE(telemetry::validate_automatic_detector_configuration(configuration));
    configuration = test_configuration();
    configuration.network_retransmit_fraction = 1.1;
    CHECK_FALSE(telemetry::validate_automatic_detector_configuration(configuration));
    configuration = test_configuration();
    configuration.network_failure_events = 0U;
    CHECK_FALSE(telemetry::validate_automatic_detector_configuration(configuration));
}

TEST_CASE("one physical disk stall observation triggers without throughput confirmation",
          "[telemetry][detector][disk][quality][delay]") {
    telemetry::AutomaticIncidentDetector detector{test_configuration()};
    auto observed = sample(1, 0.20, 0.40, 1.0, 1.0);
    observed.disk_service_time =
        telemetry::MetricValue<telemetry::Seconds>::available({0.250});

    const auto trigger = detector.observe(observed);
    REQUIRE(trigger.has_value());
    CHECK(trigger->resource == core::AutomaticIncidentResource::disk);
    CHECK(trigger->signal == core::AutomaticIncidentSignal::disk_latency);
    CHECK(detector.diagnostics().single_observation_triggers == 1U);
}

TEST_CASE("passive network drops trigger once and obey the global cooldown",
          "[telemetry][detector][network][quality][cooldown]") {
    telemetry::AutomaticIncidentDetector detector{test_configuration()};
    auto dropped = sample(1, 0.20, 0.40, 1.0, 1.0);
    dropped.network_connectivity =
        telemetry::MetricValue<telemetry::NetworkConnectivityLevel>::available(
            telemetry::NetworkConnectivityLevel::disconnected);
    const auto trigger = detector.observe(dropped);
    REQUIRE(trigger.has_value());
    CHECK(trigger->resource == core::AutomaticIncidentResource::network);
    CHECK(trigger->signal == core::AutomaticIncidentSignal::network_connectivity);

    dropped.observed_at += 1s;
    CHECK_FALSE(detector.observe(dropped));
    CHECK(detector.diagnostics().triggers_suppressed_by_cooldown == 1U);
}

TEST_CASE("quiet available quality signals do not create automatic incidents",
          "[telemetry][detector][quality][false-positive]") {
    telemetry::AutomaticIncidentDetector detector{test_configuration()};
    for (std::int64_t second = 0; second < 3'600; ++second) {
        auto quiet = sample(second, 0.20, 0.40, 1.0, 1.0);
        quiet.disk_service_time =
            telemetry::MetricValue<telemetry::Seconds>::available({0.004});
        quiet.disk_queue_depth = telemetry::MetricValue<double>::available(0.2);
        quiet.network_connectivity =
            telemetry::MetricValue<telemetry::NetworkConnectivityLevel>::available(
                telemetry::NetworkConnectivityLevel::internet);
        quiet.network_interface_changes =
            telemetry::MetricValue<std::uint64_t>::available(0U);
        quiet.network_tcp_retransmit_fraction =
            telemetry::MetricValue<telemetry::Ratio>::available({0.01});
        quiet.network_tcp_failed_connections =
            telemetry::MetricValue<std::uint64_t>::available(0U);
        quiet.network_tcp_resets =
            telemetry::MetricValue<std::uint64_t>::available(0U);
        CHECK_FALSE(detector.observe(quiet));
    }
    CHECK(detector.diagnostics().triggers_emitted == 0U);
}

TEST_CASE("severe deterministic resources trigger within three observations",
          "[telemetry][detector][delay]") {
    const struct Scenario {
        core::AutomaticIncidentResource resource;
        double cpu;
        double memory;
        double disk;
        double network;
    } scenarios[]{
        {core::AutomaticIncidentResource::cpu, 0.95, 0.40, 10.0, 10.0},
        {core::AutomaticIncidentResource::memory, 0.20, 0.98, 10.0, 10.0},
        {core::AutomaticIncidentResource::disk, 0.20, 0.40, 120.0, 10.0},
        {core::AutomaticIncidentResource::network, 0.20, 0.40, 10.0, 120.0},
    };
    for (const auto& scenario : scenarios) {
        telemetry::AutomaticIncidentDetector detector{test_configuration()};
        CHECK_FALSE(detector.observe(sample(1, scenario.cpu, scenario.memory,
                                            scenario.disk, scenario.network)));
        CHECK_FALSE(detector.observe(sample(2, scenario.cpu, scenario.memory,
                                            scenario.disk, scenario.network)));
        const auto trigger = detector.observe(sample(
            3, scenario.cpu, scenario.memory, scenario.disk, scenario.network));
        REQUIRE(trigger.has_value());
        CHECK(trigger->kind == core::IncidentTriggerKind::automatic);
        CHECK(trigger->resource == scenario.resource);
    }
}

TEST_CASE("rolling statistics detect a severe deviation below hard threshold",
          "[telemetry][detector][statistics]") {
    auto configuration = test_configuration();
    configuration.cpu = {1.0, 0.60, 0.01};
    telemetry::AutomaticIncidentDetector detector{configuration};
    for (std::int64_t second = 0; second < 10; ++second) {
        CHECK_FALSE(detector.observe(sample(second, 0.20, 0.40, 10.0, 10.0)));
    }
    CHECK_FALSE(detector.observe(sample(10, 0.70, 0.40, 10.0, 10.0)));
    CHECK_FALSE(detector.observe(sample(11, 0.70, 0.40, 10.0, 10.0)));
    const auto trigger = detector.observe(sample(12, 0.70, 0.40, 10.0, 10.0));
    REQUIRE(trigger.has_value());
    CHECK(trigger->resource == core::AutomaticIncidentResource::cpu);
    CHECK(trigger->baseline_value == Catch::Approx{0.20});
    CHECK(trigger->score > 1.0);
}

TEST_CASE("normal deterministic fixture stays inside the false-positive budget",
          "[telemetry][detector][false-positive]") {
    telemetry::AutomaticIncidentDetector detector{};
    std::size_t triggers = 0U;
    // 604,800 one-second observations accelerate seven continuous days. The
    // production false-positive budget for this smooth fixture remains zero.
    for (std::int64_t second = 0; second < 604'800; ++second) {
        const auto phase = static_cast<double>(second % 300) / 300.0;
        const auto cpu = 0.25 + 0.05 * std::sin(phase * 6.283185307179586);
        const auto memory = 0.55 + 0.02 * std::sin(phase * 3.141592653589793);
        triggers += detector.observe(sample(second, cpu, memory,
                                            8.0 * 1024.0 * 1024.0,
                                            4.0 * 1024.0 * 1024.0)).has_value();
    }
    CHECK(triggers == 0U);
}

TEST_CASE("cooldown deduplicates a sustained trigger storm",
          "[telemetry][detector][cooldown][bounded]") {
    telemetry::AutomaticIncidentDetector detector{test_configuration()};
    std::size_t triggers = 0U;
    for (std::int64_t second = 0; second < 1'000; ++second) {
        triggers += detector.observe(sample(second, 0.99, 0.40, 10.0, 10.0))
                        .has_value();
    }
    CHECK(triggers <= 9U);
    CHECK(detector.diagnostics().triggers_suppressed_by_cooldown > 900U);
}

TEST_CASE("detector storms remain inside the immutable incident queue bound",
          "[telemetry][detector][storm][storage][bounded]") {
    telemetry::AutomaticIncidentDetector detector{test_configuration()};
    core::IncidentCaptureCoordinator coordinator{2U};
    coordinator.start_accepting();
    std::size_t emitted = 0U;
    for (std::int64_t second = 0; second < 1'000; ++second) {
        const auto observed = sample(second, 0.99, 0.40, 10.0, 10.0);
        if (const auto trigger = detector.observe(observed)) {
            ++emitted;
            const auto request = coordinator.request(observed.observed_at, 0s, 0s,
                                                     *trigger);
            if (request == core::IncidentCaptureRequestResult::started) {
                const auto window = coordinator.try_begin_snapshot(observed.observed_at);
                REQUIRE(window.has_value());
                coordinator.finish_snapshot(empty_incident(*window));
            }
        }
    }
    CHECK(emitted <= 9U);
    const auto status = coordinator.status();
    CHECK(status.queue_size == 2U);
    CHECK(status.queue_capacity == 2U);
    CHECK(status.queue_rejections == emitted - 2U);
}

TEST_CASE("missing metrics break confirmation without becoming zero",
          "[telemetry][detector][availability]") {
    telemetry::AutomaticIncidentDetector detector{test_configuration()};
    CHECK_FALSE(detector.observe(sample(0, 0.99, 0.40, 10.0, 10.0)));
    auto missing = sample(1, 0.99, 0.40, 10.0, 10.0);
    missing.cpu_usage = telemetry::MetricValue<telemetry::Ratio>::unavailable(
        telemetry::MetricStatus::inaccessible);
    CHECK_FALSE(detector.observe(missing));
    CHECK_FALSE(detector.observe(sample(2, 0.99, 0.40, 10.0, 10.0)));
    CHECK_FALSE(detector.observe(sample(3, 0.99, 0.40, 10.0, 10.0)));
    CHECK(detector.observe(sample(4, 0.99, 0.40, 10.0, 10.0)).has_value());
    CHECK(detector.diagnostics().unavailable_metric_values >= 1U);
}

TEST_CASE("disabled detector resources are ignored and reconfiguration resets state",
          "[telemetry][detector][configuration][resources]") {
    auto configuration = test_configuration();
    configuration.cpu_enabled = false;
    telemetry::AutomaticIncidentDetector detector{configuration};
    for (std::int64_t second = 0; second < 6; ++second) {
        CHECK_FALSE(detector.observe(sample(second, 0.99, 0.40, 10.0, 10.0)));
    }

    configuration.cpu_enabled = true;
    REQUIRE(detector.reconfigure(configuration).has_value());
    CHECK_FALSE(detector.observe(sample(10, 0.99, 0.40, 10.0, 10.0)));
    CHECK_FALSE(detector.observe(sample(11, 0.99, 0.40, 10.0, 10.0)));
    const auto trigger = detector.observe(sample(12, 0.99, 0.40, 10.0, 10.0));
    REQUIRE(trigger.has_value());
    CHECK(trigger->resource == core::AutomaticIncidentResource::cpu);

    configuration.baseline_samples = 0U;
    CHECK_FALSE(detector.reconfigure(configuration));
}
