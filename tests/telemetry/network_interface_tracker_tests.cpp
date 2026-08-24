#include "telemetry/network_interface_tracker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace telemetry = blackbox::telemetry;

TEST_CASE("Network interface membership changes are cumulative and order independent",
          "[telemetry][network]") {
    telemetry::NetworkInterfaceTracker<4U> tracker;
    const std::array<std::uint64_t, 2U> first{10U, 20U};
    REQUIRE(tracker.update(first) ==
            telemetry::NetworkInterfaceState{2U, 0U});

    const std::array<std::uint64_t, 2U> reordered{20U, 10U};
    REQUIRE(tracker.update(reordered) ==
            telemetry::NetworkInterfaceState{2U, 0U});

    const std::array<std::uint64_t, 2U> changed{20U, 30U};
    REQUIRE(tracker.update(changed) ==
            telemetry::NetworkInterfaceState{2U, 2U});

    const std::array<std::uint64_t, 0U> disconnected{};
    REQUIRE(tracker.update(disconnected) ==
            telemetry::NetworkInterfaceState{0U, 4U});
}

TEST_CASE("Network interface tracker rejects invalid input without changing its baseline",
          "[telemetry][network]") {
    telemetry::NetworkInterfaceTracker<2U> tracker;
    const std::array<std::uint64_t, 1U> first{7U};
    REQUIRE(tracker.update(first).has_value());

    const std::array<std::uint64_t, 2U> duplicate{8U, 8U};
    CHECK_FALSE(tracker.update(duplicate).has_value());
    const std::array<std::uint64_t, 3U> overflow{8U, 9U, 10U};
    CHECK_FALSE(tracker.update(overflow).has_value());
    const std::array<std::uint64_t, 1U> invalid{0U};
    CHECK_FALSE(tracker.update(invalid).has_value());

    REQUIRE(tracker.update(first) ==
            telemetry::NetworkInterfaceState{1U, 0U});
}
