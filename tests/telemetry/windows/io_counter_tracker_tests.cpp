#include "telemetry/windows/io_counter_tracker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace telemetry = blackbox::telemetry;
namespace windows = blackbox::telemetry::windows;

TEST_CASE("I/O tracker establishes baselines and aggregates stable entity deltas",
          "[telemetry][windows][io]") {
    windows::IoCounterTracker<4U> tracker;
    const std::array first{
        windows::IoEntityCounters{1U, 100U, 200U},
        windows::IoEntityCounters{2U, 1'000U, 2'000U}};
    const auto baseline = tracker.update(first);
    REQUIRE(baseline.first.has_value());
    REQUIRE(baseline.second.has_value());
    CHECK(baseline.first.value.value == 0U);
    CHECK(baseline.second.value.value == 0U);

    const std::array second{
        windows::IoEntityCounters{1U, 130U, 260U},
        windows::IoEntityCounters{2U, 1'070U, 2'090U}};
    const auto aggregate = tracker.update(second);
    REQUIRE(aggregate.first.has_value());
    REQUIRE(aggregate.second.has_value());
    CHECK(aggregate.first.value.value == 100U);
    CHECK(aggregate.second.value.value == 150U);
}

TEST_CASE("I/O tracker suppresses arrival removal and reappearance spikes",
          "[telemetry][windows][io]") {
    windows::IoCounterTracker<2U> tracker;
    const std::array baseline{
        windows::IoEntityCounters{1U, 100U, 200U},
        windows::IoEntityCounters{2U, 300U, 400U}};
    static_cast<void>(tracker.update(baseline));

    const std::array churn{
        windows::IoEntityCounters{2U, 320U, 430U},
        windows::IoEntityCounters{3U, 9'000'000U, 8'000'000U}};
    const auto after_arrival = tracker.update(churn);
    REQUIRE(after_arrival.first.has_value());
    REQUIRE(after_arrival.second.has_value());
    CHECK(after_arrival.first.value.value == 20U);
    CHECK(after_arrival.second.value.value == 30U);

    const std::array removed{windows::IoEntityCounters{3U, 9'000'010U, 8'000'020U}};
    const auto after_removal = tracker.update(removed);
    CHECK(after_removal.first.value.value == 30U);
    CHECK(after_removal.second.value.value == 50U);

    const std::array reappeared{windows::IoEntityCounters{1U, 500'000U, 600'000U}};
    const auto after_reappearance = tracker.update(reappeared);
    CHECK(after_reappearance.first.value.value == 30U);
    CHECK(after_reappearance.second.value.value == 50U);
}

TEST_CASE("I/O tracker reports reset only on the affected channel and recovers",
          "[telemetry][windows][io]") {
    windows::IoCounterTracker<2U> tracker;
    const std::array baseline{windows::IoEntityCounters{7U, 100U, 200U}};
    static_cast<void>(tracker.update(baseline));

    const std::array reset{windows::IoEntityCounters{7U, 10U, 250U}};
    const auto reset_result = tracker.update(reset);
    CHECK(reset_result.first.status == telemetry::MetricStatus::temporarily_unavailable);
    REQUIRE(reset_result.second.has_value());
    CHECK(reset_result.second.value.value == 50U);

    const std::array recovered{windows::IoEntityCounters{7U, 25U, 270U}};
    const auto recovered_result = tracker.update(recovered);
    REQUIRE(recovered_result.first.has_value());
    REQUIRE(recovered_result.second.has_value());
    CHECK(recovered_result.first.value.value == 15U);
    CHECK(recovered_result.second.value.value == 70U);
}

TEST_CASE("I/O tracker rejects ambiguous input and aggregate overflow",
          "[telemetry][windows][io]") {
    windows::IoCounterTracker<2U> duplicate_tracker;
    const std::array duplicates{
        windows::IoEntityCounters{1U, 10U, 20U},
        windows::IoEntityCounters{1U, 30U, 40U}};
    const auto duplicate = duplicate_tracker.update(duplicates);
    CHECK_FALSE(duplicate.first.has_value());
    CHECK_FALSE(duplicate.second.has_value());

    windows::IoCounterTracker<2U> overflow_tracker;
    const std::array zero{
        windows::IoEntityCounters{1U, 0U, 0U},
        windows::IoEntityCounters{2U, 0U, 0U}};
    static_cast<void>(overflow_tracker.update(zero));
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const std::array overflowing{
        windows::IoEntityCounters{1U, maximum, 1U},
        windows::IoEntityCounters{2U, 1U, 1U}};
    const auto result = overflow_tracker.update(overflowing);
    CHECK(result.first.status == telemetry::MetricStatus::temporarily_unavailable);
    REQUIRE(result.second.has_value());
    CHECK(result.second.value.value == 2U);
}
