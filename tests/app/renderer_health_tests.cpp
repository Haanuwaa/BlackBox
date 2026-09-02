#include "app/renderer_health.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;

TEST_CASE("renderer health reports app-scoped timing and hitches") {
    blackbox::app::RendererHealthTracker tracker;
    tracker.observe({10ms, 2ms, 16ms, true});
    tracker.observe({60ms, 2ms, 16ms, true});
    tracker.observe({8ms, 1ms, 16ms, false});

    const auto snapshot = tracker.snapshot();
    CHECK(snapshot.frames == 3U);
    CHECK(snapshot.hitches == 1U);
    CHECK(snapshot.present_failures == 1U);
    CHECK(snapshot.frame_p95_milliseconds == 62.0);
    CHECK(snapshot.frame_maximum_milliseconds == 62.0);
}

TEST_CASE("renderer health rejects invalid observations and resets rolling timings") {
    blackbox::app::RendererHealthTracker tracker;
    tracker.observe({-1ms, 1ms, 16ms, true});
    tracker.observe({1ms, 1ms, 0ms, true});
    CHECK(tracker.snapshot().frames == 0U);

    tracker.observe({2ms, 1ms, 16ms, true});
    tracker.reset_window();
    const auto snapshot = tracker.snapshot();
    CHECK(snapshot.frames == 1U);
    CHECK(snapshot.frame_p95_milliseconds == 0.0);
}
