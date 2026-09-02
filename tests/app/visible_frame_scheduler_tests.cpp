#include "app/visible_frame_scheduler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace blackbox::app {
namespace {

using namespace std::chrono_literals;

TEST_CASE("visible frame scheduler renders immediately and caps repeated frames",
          "[app][ui][performance]") {
    VisibleFrameScheduler scheduler;
    const auto start = core::MonotonicTimePoint{1s};

    CHECK(scheduler.frame_due(start));
    CHECK_FALSE(scheduler.frame_due(start + 1ms));
    CHECK(scheduler.wait_timeout(start + 1ms) == 32ms);
    CHECK_FALSE(scheduler.frame_due(start + 32ms));
    CHECK(scheduler.wait_timeout(start + 32ms) == 1ms);
    CHECK(scheduler.frame_due(start + visible_idle_frame_interval));
}

TEST_CASE("visible frame scheduler raises cadence only around direct interaction",
          "[app][ui][performance][interaction]") {
    VisibleFrameScheduler scheduler;
    const auto start = core::MonotonicTimePoint{4s};
    REQUIRE(scheduler.frame_due(start));

    scheduler.note_interaction(start + 1ms);
    CHECK(scheduler.wait_timeout(start + 1ms) == 16ms);
    CHECK_FALSE(scheduler.frame_due(start + 16ms));
    CHECK(scheduler.frame_due(start + 17ms));
    CHECK(scheduler.wait_timeout(start + 17ms) == 16ms);

    // Once interaction has been quiet for the hold period, newly scheduled
    // frames return to the 30 Hz idle ceiling.
    CHECK(scheduler.frame_due(start + 318ms));
    CHECK(scheduler.wait_timeout(start + 318ms) == 33ms);
}

TEST_CASE("repeated interaction extends but never bypasses the frame ceiling",
          "[app][ui][performance][interaction]") {
    VisibleFrameScheduler scheduler;
    const auto start = core::MonotonicTimePoint{5s};
    REQUIRE(scheduler.frame_due(start));
    scheduler.note_interaction(start + 1ms);
    scheduler.note_interaction(start + 10ms);
    scheduler.note_interaction(start + 12ms);

    CHECK_FALSE(scheduler.frame_due(start + 15ms));
    CHECK(scheduler.frame_due(start + 17ms));
    CHECK_FALSE(scheduler.frame_due(start + 18ms));
}

TEST_CASE("visible frame scheduler skips stale deadlines without catch-up bursts",
          "[app][ui][performance]") {
    VisibleFrameScheduler scheduler;
    const auto start = core::MonotonicTimePoint{2s};
    REQUIRE(scheduler.frame_due(start));

    CHECK(scheduler.frame_due(start + 100ms));
    CHECK_FALSE(scheduler.frame_due(start + 101ms));
    CHECK(scheduler.wait_timeout(start + 101ms) == 32ms);
}

TEST_CASE("visible frame scheduler reset makes a restored window render immediately",
          "[app][ui][performance]") {
    VisibleFrameScheduler scheduler;
    const auto start = core::MonotonicTimePoint{3s};
    REQUIRE(scheduler.frame_due(start));
    REQUIRE_FALSE(scheduler.frame_due(start + 10ms));

    scheduler.reset();

    CHECK(scheduler.wait_timeout(start + 10ms) == 0ms);
    CHECK(scheduler.frame_due(start + 10ms));
}

} // namespace
} // namespace blackbox::app
