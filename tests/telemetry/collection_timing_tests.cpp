#include "telemetry/collection_timing.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace telemetry = blackbox::telemetry;
using namespace std::chrono_literals;

TEST_CASE("collection timing reports bounded nearest-rank percentiles",
          "[telemetry][timing]") {
    telemetry::CollectionTimingWindow timing;
    for (std::int64_t value = 1; value <= 100; ++value) {
        timing.record(std::chrono::microseconds{value});
    }

    const auto summary = timing.summary();
    CHECK(summary.samples_recorded == 100U);
    CHECK(summary.samples_in_window == 100U);
    CHECK(summary.average == 50'500ns);
    CHECK(summary.p50 == 50us);
    CHECK(summary.p95 == 95us);
    CHECK(summary.p99 == 99us);
    CHECK(summary.maximum == 100us);
}

TEST_CASE("collection timing overwrites old values without growing",
          "[telemetry][timing]") {
    telemetry::CollectionTimingWindow timing;
    for (std::int64_t value = 1; value <= 300; ++value) {
        timing.record(std::chrono::nanoseconds{value});
    }

    const auto summary = timing.summary();
    CHECK(summary.samples_recorded == 300U);
    CHECK(summary.samples_in_window == telemetry::CollectionTimingWindow::capacity);
    CHECK(summary.average == 172ns);
    CHECK(summary.p50 == 172ns);
    CHECK(summary.p95 == 288ns);
    CHECK(summary.p99 == 298ns);
    CHECK(summary.maximum == 300ns);
}

TEST_CASE("collection timing clamps negative values and resets", "[telemetry][timing]") {
    telemetry::CollectionTimingWindow timing;
    timing.record(-10ns);
    CHECK(timing.summary().maximum == 0ns);

    timing.reset();
    CHECK(timing.summary() == telemetry::CollectionTimingSummary{});
}
