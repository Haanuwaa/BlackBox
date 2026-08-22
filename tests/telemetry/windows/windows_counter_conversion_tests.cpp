#include "telemetry/windows/windows_counter_conversion.hpp"
#include "telemetry/normalizer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

namespace telemetry = blackbox::telemetry;
namespace windows = blackbox::telemetry::windows;

TEST_CASE("Windows system times remove idle from kernel exactly once",
          "[telemetry][windows]") {
    const auto counters = windows::convert_system_times(
        200U,
        500U,
        300U);

    REQUIRE(counters.has_value());
    CHECK(counters.value.busy_ticks == 600U);
    CHECK(counters.value.total_ticks == 800U);
}

TEST_CASE("Windows system time conversion rejects inconsistent fixtures",
          "[telemetry][windows]") {
    const auto idle_above_kernel = windows::convert_system_times(501U, 500U, 300U);
    const auto total_overflow = windows::convert_system_times(
        0U,
        std::numeric_limits<std::uint64_t>::max(),
        1U);

    CHECK(idle_above_kernel.status == telemetry::MetricStatus::temporarily_unavailable);
    CHECK(total_overflow.status == telemetry::MetricStatus::temporarily_unavailable);
}

TEST_CASE("captured Windows counter fixtures normalize to expected utilization",
          "[telemetry][windows]") {
    const auto previous = windows::convert_system_times(
        8'000'000U,
        12'000'000U,
        4'000'000U);
    const auto current = windows::convert_system_times(
        8'600'000U,
        13'000'000U,
        4'500'000U);

    const auto usage = telemetry::normalize_cpu_usage(previous, current,
                                                       std::chrono::seconds{1});
    REQUIRE(usage.has_value());
    // Total delta is 1.5M ticks and busy delta is 0.9M ticks.
    CHECK(usage.value.value == 0.6);
}
