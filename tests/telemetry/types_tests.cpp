#include "telemetry/types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace telemetry = blackbox::telemetry;

static_assert(std::is_trivially_copyable_v<telemetry::MetricValue<telemetry::ByteCount>>);
static_assert(std::is_trivially_copyable_v<telemetry::SystemSample>);

TEST_CASE("metric values distinguish availability reasons", "[telemetry][types]") {
    const auto available = telemetry::MetricValue<telemetry::ByteCount>::available(
        telemetry::ByteCount{42U});
    const auto unsupported = telemetry::MetricValue<telemetry::ByteCount>::unavailable(
        telemetry::MetricStatus::unsupported);
    const auto inaccessible = telemetry::MetricValue<telemetry::ByteCount>::unavailable(
        telemetry::MetricStatus::inaccessible);
    const auto temporary = telemetry::MetricValue<telemetry::ByteCount>::unavailable(
        telemetry::MetricStatus::temporarily_unavailable);

    CHECK(available.has_value());
    CHECK(available.value.value == 42U);
    CHECK_FALSE(unsupported.has_value());
    CHECK(unsupported.status == telemetry::MetricStatus::unsupported);
    CHECK(inaccessible.status == telemetry::MetricStatus::inaccessible);
    CHECK(temporary.status == telemetry::MetricStatus::temporarily_unavailable);
}
TEST_CASE("sampling tiers can be requested independently", "[telemetry][types]") {
    const telemetry::SamplingTierSet fast_and_slow =
        telemetry::SamplingTier::fast | telemetry::SamplingTier::slow;

    CHECK(fast_and_slow.contains(telemetry::SamplingTier::fast));
    CHECK_FALSE(fast_and_slow.contains(telemetry::SamplingTier::normal));
    CHECK(fast_and_slow.contains(telemetry::SamplingTier::slow));
    CHECK(telemetry::SamplingTierSet::all().contains(telemetry::SamplingTier::normal));
}

TEST_CASE("process identity prevents PID reuse collisions", "[telemetry][types]") {
    constexpr telemetry::ProcessIdentity first{telemetry::ProcessId{99U}, 1000U};
    constexpr telemetry::ProcessIdentity reused{telemetry::ProcessId{99U}, 2000U};

    STATIC_CHECK(first != reused);
}
