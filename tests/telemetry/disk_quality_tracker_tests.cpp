#include "telemetry/disk_quality_tracker.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>

namespace telemetry = blackbox::telemetry;
using namespace std::chrono_literals;

TEST_CASE("Disk quality tracker derives exact interval means and average queue",
          "[telemetry][disk-quality]") {
    telemetry::DiskQualityTracker<4U> tracker;
    const auto start = blackbox::core::MonotonicTimePoint{} + 1s;
    std::array counters{telemetry::DiskQualityCounters{
        7U, 10U, 20U, 100'000'000U, 400'000'000U, 500'000'000U}};

    const auto first = tracker.update(start, counters);
    CHECK(first.read_latency.status == telemetry::MetricStatus::temporarily_unavailable);
    CHECK(first.queue_depth.status == telemetry::MetricStatus::temporarily_unavailable);

    counters[0].read_operations += 2U;
    counters[0].write_operations += 1U;
    counters[0].read_time_nanoseconds += 40'000'000U;
    counters[0].write_time_nanoseconds += 60'000'000U;
    *counters[0].weighted_time_nanoseconds += 750'000'000U;
    const auto result = tracker.update(start + 1s, counters);

    REQUIRE(result.read_latency.has_value());
    REQUIRE(result.write_latency.has_value());
    REQUIRE(result.service_time.has_value());
    REQUIRE(result.queue_depth.has_value());
    CHECK(result.read_latency.value.value == Catch::Approx{0.020});
    CHECK(result.write_latency.value.value == Catch::Approx{0.060});
    CHECK(result.service_time.value.value == Catch::Approx{0.1 / 3.0});
    CHECK(result.queue_depth.value == Catch::Approx{0.75});
    REQUIRE(result.worst_device_id.has_value());
    CHECK(result.worst_device_id.value == 7U);
}

TEST_CASE("Disk quality tracker warms lifecycle changes and rejects counter resets",
          "[telemetry][disk-quality]") {
    telemetry::DiskQualityTracker<2U> tracker;
    const auto start = blackbox::core::MonotonicTimePoint{} + 1s;
    std::array first{telemetry::DiskQualityCounters{
        1U, 1U, 1U, 10U, 10U, 10U}};
    static_cast<void>(tracker.update(start, first));

    std::array replacement{telemetry::DiskQualityCounters{
        2U, 50U, 50U, 500U, 500U, 500U}};
    const auto arrival = tracker.update(start + 1s, replacement);
    CHECK(arrival.service_time.status == telemetry::MetricStatus::temporarily_unavailable);

    replacement[0].read_operations = 1U;
    replacement[0].read_time_nanoseconds = 1U;
    const auto reset = tracker.update(start + 2s, replacement);
    CHECK(reset.service_time.status == telemetry::MetricStatus::temporarily_unavailable);
}

TEST_CASE("Disk quality tracker preserves unsupported queue and selects worst device",
          "[telemetry][disk-quality]") {
    telemetry::DiskQualityTracker<4U> tracker;
    const auto start = blackbox::core::MonotonicTimePoint{} + 1s;
    std::array counters{
        telemetry::DiskQualityCounters{1U, 1U, 0U, 10U, 0U, std::nullopt},
        telemetry::DiskQualityCounters{2U, 1U, 0U, 10U, 0U, std::nullopt}};
    static_cast<void>(tracker.update(start, counters));
    counters[0].read_operations += 1U;
    counters[0].read_time_nanoseconds += 10'000'000U;
    counters[1].read_operations += 1U;
    counters[1].read_time_nanoseconds += 80'000'000U;

    const auto result = tracker.update(start + 1s, counters);
    CHECK(result.queue_depth.status == telemetry::MetricStatus::unsupported);
    REQUIRE(result.worst_device_id.has_value());
    CHECK(result.worst_device_id.value == 2U);
    CHECK(result.read_latency.value.value == Catch::Approx{0.080});
}
