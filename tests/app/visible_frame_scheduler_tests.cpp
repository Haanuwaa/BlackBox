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
    CHECK(scheduler.frame_due(start + visible_frame_interval));
}

TEST_CASE("visible frame scheduler skips stale deadlines without catch-up bursts",
          "[app][ui][performance]") {
    VisibleFrameScheduler scheduler;
    const auto start = core::MonotonicTimePoint{2s};
    REQUIRE(scheduler.frame_due(start));

    CHECK(scheduler.frame_due(start + 100ms));
    CHECK_FALSE(scheduler.frame_due(start + 101ms));
    CHECK(scheduler.wait_timeout(start + 101ms) == 31ms);
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
