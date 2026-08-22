#include "core/circular_recorder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <type_traits>

namespace core = blackbox::core;

TEST_CASE("circular recorder handles empty and zero-capacity histories",
          "[core][recorder]") {
    core::CircularRecorder<int> recorder{0U};
    CHECK(recorder.snapshot().empty());
    recorder.append(7);

    const auto statistics = recorder.statistics();
    CHECK(statistics.capacity == 0U);
    CHECK(statistics.size == 0U);
    CHECK(statistics.total_appends == 1U);
    CHECK(statistics.discarded_samples == 1U);
    CHECK(statistics.overwritten_samples == 0U);
    CHECK(statistics.utilization() == 0.0);
}

TEST_CASE("circular recorder snapshots partial histories chronologically",
          "[core][recorder]") {
    core::CircularRecorder<int> recorder{4U};
    recorder.append(10);
    recorder.append(20);

    const auto snapshot = recorder.snapshot();
    REQUIRE(snapshot.size() == 2U);
    CHECK(snapshot.samples()[0] == 10);
    CHECK(snapshot.samples()[1] == 20);
    CHECK(recorder.statistics().utilization() == 0.5);
    using SnapshotElement = decltype(snapshot.samples().front());
    STATIC_REQUIRE(std::is_const_v<std::remove_reference_t<SnapshotElement>>);
}

TEST_CASE("full circular recorder overwrites oldest samples at capacities one and N",
          "[core][recorder]") {
    core::CircularRecorder<int> single{1U};
    single.append(1);
    single.append(2);
    REQUIRE(single.snapshot().size() == 1U);
    CHECK(single.snapshot().samples().front() == 2);
    CHECK(single.statistics().overwritten_samples == 1U);

    core::CircularRecorder<int> recorder{3U};
    for (int value = 1; value <= 8; ++value) {
        recorder.append(value);
    }
    const auto snapshot = recorder.snapshot();
    REQUIRE(snapshot.size() == 3U);
    CHECK(snapshot.samples()[0] == 6);
    CHECK(snapshot.samples()[1] == 7);
    CHECK(snapshot.samples()[2] == 8);
    CHECK(recorder.statistics().overwritten_samples == 5U);
}

TEST_CASE("bounded snapshots return only the newest requested samples",
          "[core][recorder]") {
    core::CircularRecorder<int> recorder{5U};
    for (int value = 1; value <= 5; ++value) {
        recorder.append(value);
    }
    const auto latest = recorder.snapshot(2U);
    REQUIRE(latest.size() == 2U);
    CHECK(latest.samples()[0] == 4);
    CHECK(latest.samples()[1] == 5);
    CHECK(recorder.snapshot(0U).empty());
}

TEST_CASE("reconfiguration clears history and starts a new epoch",
          "[core][recorder]") {
    core::CircularRecorder<int> recorder{2U};
    recorder.append(1);
    recorder.append(2);
    recorder.reconfigure(4U);

    CHECK(recorder.snapshot().empty());
    CHECK(recorder.snapshot().epoch() == 1U);
    CHECK(recorder.statistics().capacity == 4U);
    CHECK(recorder.statistics().total_appends == 0U);
    recorder.append(3);
    CHECK(recorder.snapshot().samples().front() == 3);
}

TEST_CASE("concurrent bounded readers see ordered immutable recorder snapshots",
          "[core][recorder][concurrency]") {
    core::CircularRecorder<int> recorder{32U};
    std::atomic<bool> writer_done{};
    std::atomic<bool> ordering_failed{};

    std::jthread writer{[&] {
        for (int value = 1; value <= 10'000; ++value) {
            recorder.append(value);
        }
        writer_done.store(true);
    }};
    std::jthread reader{[&] {
        while (!writer_done.load()) {
            const auto snapshot = recorder.snapshot(16U);
            const auto samples = snapshot.samples();
            for (std::size_t index = 1U; index < samples.size(); ++index) {
                if (samples[index] <= samples[index - 1U]) {
                    ordering_failed.store(true);
                }
            }
        }
    }};

    writer.join();
    reader.join();
    CHECK_FALSE(ordering_failed.load());
    CHECK(recorder.statistics().size == 32U);
}

TEST_CASE("seven-day accelerated recording remains fixed-capacity and chronological",
          "[core][recorder][multi-day][soak]") {
    constexpr std::uint64_t seven_days_at_one_second = 7U * 24U * 60U * 60U;
    core::CircularRecorder<std::uint64_t> recorder{300U};
    for (std::uint64_t second = 0U; second < seven_days_at_one_second; ++second) {
        recorder.append(second);
    }
    const auto snapshot = recorder.snapshot();
    REQUIRE(snapshot.size() == 300U);
    CHECK(snapshot.samples().front() == seven_days_at_one_second - 300U);
    CHECK(snapshot.samples().back() == seven_days_at_one_second - 1U);
    const auto statistics = recorder.statistics();
    CHECK(statistics.capacity == 300U);
    CHECK(statistics.total_appends == seven_days_at_one_second);
    CHECK(statistics.overwritten_samples == seven_days_at_one_second - 300U);
    CHECK(statistics.utilization() == 1.0);
}
