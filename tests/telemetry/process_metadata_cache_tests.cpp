#include "telemetry/process_metadata_cache.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>

namespace telemetry = blackbox::telemetry;
using namespace std::chrono_literals;

namespace {

telemetry::ProcessInfo info(const std::uint32_t pid,
                            const std::uint64_t token,
                            const char* name) {
    telemetry::ProcessInfo result{};
    result.identity = telemetry::ProcessIdentity{telemetry::ProcessId{pid}, token};
    result.parent_pid = telemetry::MetricValue<telemetry::ProcessId>::available(
        telemetry::ProcessId{1U});
    result.name = telemetry::MetricValue<std::string>::available(name);
    result.executable_path = telemetry::MetricValue<std::string>::unavailable(
        telemetry::MetricStatus::temporarily_unavailable);
    return result;
}

telemetry::ProcessSample active(const telemetry::ProcessIdentity identity) {
    telemetry::ProcessSample result{};
    result.identity = identity;
    return result;
}

} // namespace

TEST_CASE("metadata cache keeps exited identities through history retention",
          "[telemetry][process][metadata]") {
    telemetry::ProcessMetadataCache cache{5s, 4U};
    const auto first = info(10U, 100U, "first");
    const std::array metadata{first};
    const std::array active_processes{active(first.identity)};
    cache.update(metadata, active_processes, std::chrono::steady_clock::time_point{1s});
    CHECK(cache.size() == 1U);

    cache.update({}, {}, std::chrono::steady_clock::time_point{6s});
    CHECK(cache.size() == 1U);
    cache.update({}, {}, std::chrono::steady_clock::time_point{7s});
    CHECK(cache.size() == 0U);
}

TEST_CASE("metadata cache never merges reused PIDs and remains bounded",
          "[telemetry][process][metadata]") {
    telemetry::ProcessMetadataCache cache{1h, 2U};
    const auto old_process = info(42U, 1U, "old");
    const auto new_process = info(42U, 2U, "new");
    const auto other = info(99U, 1U, "other");

    const std::array old_metadata{old_process};
    const std::array old_active{active(old_process.identity)};
    cache.update(old_metadata, old_active, std::chrono::steady_clock::time_point{1s});
    cache.update({}, {}, std::chrono::steady_clock::time_point{2s});

    const std::array new_metadata{new_process, other};
    const std::array new_active{active(new_process.identity), active(other.identity)};
    cache.update(new_metadata, new_active, std::chrono::steady_clock::time_point{3s});
    CHECK(cache.size() == 2U);
    CHECK(cache.evictions() == 1U);
    const auto snapshot = cache.snapshot();
    CHECK(snapshot[0].identity != old_process.identity);
    CHECK(snapshot[1].identity != old_process.identity);
}

TEST_CASE("metadata cache refuses growth when every bounded entry is active",
          "[telemetry][process][metadata]") {
    telemetry::ProcessMetadataCache cache{1h, 1U};
    const auto first = info(1U, 1U, "one");
    const auto second = info(2U, 1U, "two");
    const std::array metadata{first, second};
    const std::array active_processes{active(first.identity), active(second.identity)};
    cache.update(metadata, active_processes, std::chrono::steady_clock::time_point{1s});
    CHECK(cache.size() == 1U);
}
