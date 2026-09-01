#include "telemetry/network_interface_tracker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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

TEST_CASE("Network interface tracker matches a randomized hotplug and reconnect model",
          "[telemetry][network][property][hotplug][reconnect]") {
    telemetry::NetworkInterfaceTracker<8U> tracker;
    std::vector<std::uint64_t> previous{};
    std::uint64_t expected_changes{};
    std::uint64_t random_state{0x9e3779b97f4a7c15ULL};
    auto next_random = [&]() {
        random_state ^= random_state << 7U;
        random_state ^= random_state >> 9U;
        random_state ^= random_state << 8U;
        return random_state;
    };

    for (std::size_t iteration = 0U; iteration < 4'096U; ++iteration) {
        std::array<std::uint64_t, 10U> storage{};
        const auto requested = static_cast<std::size_t>(next_random() % storage.size());
        std::size_t count{};
        for (; count < requested; ++count) {
            storage[count] = 1U + next_random() % 12U;
            if (std::find(storage.begin(), storage.begin() + static_cast<std::ptrdiff_t>(count),
                          storage[count]) != storage.begin() + static_cast<std::ptrdiff_t>(count)) {
                --count;
            }
        }
        const auto current = std::span<const std::uint64_t>{storage.data(), requested};
        const auto result = tracker.update(current);
        if (requested > 8U) {
            CHECK_FALSE(result.has_value());
            continue;
        }

        REQUIRE(result.has_value());
        if (!previous.empty() || iteration != 0U) {
            for (const auto identity : current) {
                if (std::find(previous.begin(), previous.end(), identity) == previous.end()) {
                    ++expected_changes;
                }
            }
            for (const auto identity : previous) {
                if (std::find(current.begin(), current.end(), identity) == current.end()) {
                    ++expected_changes;
                }
            }
        }
        previous.assign(current.begin(), current.end());
        CHECK(*result == telemetry::NetworkInterfaceState{
                             static_cast<std::uint64_t>(requested), expected_changes});
    }
}
