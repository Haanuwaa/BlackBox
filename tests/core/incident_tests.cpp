#include "core/incident.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <vector>

namespace core = blackbox::core;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] std::shared_ptr<const core::IncidentSnapshot> empty_incident(
    const core::IncidentCaptureWindow window) {
    core::IncidentHeader header{};
    header.window = window;
    return std::make_shared<const core::IncidentSnapshot>(
        header, std::vector<core::IncidentSystemSample>{},
        std::vector<core::IncidentProcessInfo>{},
        std::vector<core::IncidentProcessSample>{});
}

} // namespace

TEST_CASE("incident capture merges overlapping post-windows",
          "[core][incident][clock]") {
    core::IncidentCaptureCoordinator coordinator{2U};
    const auto first_event = core::MonotonicTimePoint{100s};

    CHECK(coordinator.request(first_event, 120s, 30s) ==
          core::IncidentCaptureRequestResult::stopped);
    coordinator.start_accepting();
    CHECK(coordinator.request(first_event, 120s, 30s) ==
          core::IncidentCaptureRequestResult::started);
    CHECK(coordinator.request(first_event + 20s, 120s, 30s) ==
          core::IncidentCaptureRequestResult::merged);

    const auto active = coordinator.status();
    REQUIRE(active.has_pending_window);
    CHECK(active.pending_window.event_time == first_event);
    CHECK(active.pending_window.requested_start == first_event - 120s);
    CHECK(active.pending_window.requested_end == first_event + 50s);
    CHECK(active.pending_window.trigger_count == 2U);
    CHECK(active.capture_requests_merged == 1U);
    CHECK_FALSE(coordinator.try_begin_snapshot(first_event + 49s).has_value());

    const auto due = coordinator.try_begin_snapshot(first_event + 50s);
    REQUIRE(due.has_value());
    CHECK(coordinator.status().phase ==
          core::IncidentCapturePhase::constructing_snapshot);
    coordinator.finish_snapshot(empty_incident(*due));
    CHECK(coordinator.status().incidents_completed == 1U);
    CHECK(coordinator.status().queue_size == 1U);
}

TEST_CASE("automatic and manual capture provenance survives overlap merging",
          "[core][incident][automatic][overlap]") {
    core::IncidentCaptureCoordinator coordinator{2U};
    coordinator.start_accepting();
    const auto event = core::MonotonicTimePoint{100s};
    const core::IncidentCaptureTrigger automatic{
        core::IncidentTriggerKind::automatic,
        core::AutomaticIncidentResource::cpu, 0.99, 0.30, 2.5};
    REQUIRE(coordinator.request(event, 120s, 30s, automatic) ==
            core::IncidentCaptureRequestResult::started);
    REQUIRE(coordinator.request(event + 5s, 120s, 30s) ==
            core::IncidentCaptureRequestResult::merged);
    const auto pending = coordinator.status().pending_window;
    CHECK(pending.trigger_count == 2U);
    CHECK(pending.manual_trigger_count == 1U);
    CHECK(pending.automatic_trigger_count == 1U);
    CHECK(pending.automatic_resource == core::AutomaticIncidentResource::cpu);
    CHECK(pending.automatic_observed_value == 0.99);
    CHECK(pending.automatic_baseline_value == 0.30);
    CHECK(pending.automatic_score == 2.5);
}

TEST_CASE("incident writer handoff rejects saturation without growing",
          "[core][incident][queue]") {
    core::IncidentCaptureCoordinator coordinator{1U};
    coordinator.start_accepting();
    const auto event = core::MonotonicTimePoint{10s};
    REQUIRE(coordinator.request(event, 0s, 0s) ==
            core::IncidentCaptureRequestResult::started);
    const auto due = coordinator.try_begin_snapshot(event);
    REQUIRE(due.has_value());
    coordinator.finish_snapshot(empty_incident(*due));

    CHECK(coordinator.request(event + 1s, 0s, 0s) ==
          core::IncidentCaptureRequestResult::queue_full);
    auto saturated = coordinator.status();
    CHECK(saturated.queue_size == 1U);
    CHECK(saturated.queue_capacity == 1U);
    CHECK(saturated.queue_rejections == 1U);
    CHECK_FALSE(saturated.can_request);

    const auto work = coordinator.try_pop();
    REQUIRE(work != nullptr);
    CHECK(work->header().window.sequence == due->sequence);
    CHECK(coordinator.status().queue_size == 0U);
    CHECK(coordinator.request(event + 2s, 0s, 0s) ==
          core::IncidentCaptureRequestResult::started);
}

TEST_CASE("shutdown cancels an incomplete capture and rejects new work",
          "[core][incident][shutdown]") {
    core::IncidentCaptureCoordinator coordinator{2U};
    coordinator.start_accepting();
    REQUIRE(coordinator.request(core::MonotonicTimePoint{5s}, 2s, 3s) ==
            core::IncidentCaptureRequestResult::started);

    coordinator.stop_accepting();
    const auto status = coordinator.status();
    CHECK(status.phase == core::IncidentCapturePhase::stopped);
    CHECK(status.captures_cancelled == 1U);
    CHECK_FALSE(status.has_pending_window);
    CHECK(coordinator.request(core::MonotonicTimePoint{6s}, 2s, 3s) ==
          core::IncidentCaptureRequestResult::stopped);
}

TEST_CASE("snapshot construction failure releases its bounded queue slot",
          "[core][incident][failure]") {
    core::IncidentCaptureCoordinator coordinator{1U};
    coordinator.start_accepting();
    const auto event = core::MonotonicTimePoint{5s};
    REQUIRE(coordinator.request(event, 0s, 0s) ==
            core::IncidentCaptureRequestResult::started);
    REQUIRE(coordinator.try_begin_snapshot(event).has_value());
    coordinator.finish_snapshot({});

    const auto status = coordinator.status();
    CHECK(status.snapshot_failures == 1U);
    CHECK(status.queue_size == 0U);
    CHECK(status.can_request);
}
