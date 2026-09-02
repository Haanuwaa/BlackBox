#include "telemetry/foreground_application_tracker.hpp"

#include <catch2/catch_test_macros.hpp>

namespace telemetry = blackbox::telemetry;

TEST_CASE("opaque foreground tracker fails closed for ambiguity and source loss",
          "[telemetry][foreground][wayland]") {
    telemetry::ForegroundApplicationTracker<2U> tracker;

    CHECK(tracker.current(7U, telemetry::MetricStatus::unsupported).status ==
          telemetry::MetricStatus::unsupported);
    REQUIRE(tracker.add(10U));
    REQUIRE(tracker.set_application_token(10U, 100U));
    REQUIRE(tracker.set_active(10U, true));
    const auto first_identity = telemetry::OpaqueApplicationIdentity{7U, 100U};
    CHECK(tracker.current(7U, telemetry::MetricStatus::available).value == first_identity);

    REQUIRE(tracker.add(20U));
    REQUIRE(tracker.set_application_token(20U, 100U));
    REQUIRE(tracker.set_active(20U, true));
    CHECK(tracker.current(7U, telemetry::MetricStatus::available).value == first_identity);

    REQUIRE(tracker.set_application_token(20U, 200U));
    CHECK(tracker.current(7U, telemetry::MetricStatus::available).status ==
          telemetry::MetricStatus::temporarily_unavailable);

    tracker.remove(10U);
    const auto second_identity = telemetry::OpaqueApplicationIdentity{7U, 200U};
    CHECK(tracker.current(7U, telemetry::MetricStatus::available).value == second_identity);
    tracker.reset();
    CHECK(tracker.current(7U, telemetry::MetricStatus::available).status ==
          telemetry::MetricStatus::temporarily_unavailable);
}

TEST_CASE("opaque foreground tracker is bounded and rejects invalid identifiers",
          "[telemetry][foreground][wayland]") {
    telemetry::ForegroundApplicationTracker<1U> tracker;
    CHECK_FALSE(tracker.add(0U));
    REQUIRE(tracker.add(1U));
    CHECK_FALSE(tracker.add(2U));
    CHECK_FALSE(tracker.set_application_token(1U, 0U));
    CHECK_FALSE(tracker.set_active(2U, true));
}
