#include "telemetry/normalizer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace telemetry = blackbox::telemetry;
using Catch::Approx;
using namespace std::chrono_literals;

namespace {

telemetry::RawTelemetrySnapshot
snapshot(const std::chrono::steady_clock::time_point observed_at, const std::uint64_t busy,
         const std::uint64_t total, const std::uint64_t memory_total,
         const std::uint64_t memory_available, const std::uint64_t disk_read,
         const std::uint64_t disk_write, const std::uint64_t network_receive,
         const std::uint64_t network_transmit) {
    telemetry::RawTelemetrySnapshot result{};
    result.observed_at = observed_at;
    result.sampled_tiers = telemetry::SamplingTierSet::all();
    result.system.cpu_time = telemetry::MetricValue<telemetry::CpuTimeCounters>::available(
        telemetry::CpuTimeCounters{busy, total});
    result.system.memory_total =
        telemetry::MetricValue<telemetry::ByteCount>::available(telemetry::ByteCount{memory_total});
    result.system.memory_available = telemetry::MetricValue<telemetry::ByteCount>::available(
        telemetry::ByteCount{memory_available});
    result.system.disk_read_bytes =
        telemetry::MetricValue<telemetry::ByteCount>::available(telemetry::ByteCount{disk_read});
    result.system.disk_write_bytes =
        telemetry::MetricValue<telemetry::ByteCount>::available(telemetry::ByteCount{disk_write});
    result.system.network_receive_bytes = telemetry::MetricValue<telemetry::ByteCount>::available(
        telemetry::ByteCount{network_receive});
    result.system.network_transmit_bytes = telemetry::MetricValue<telemetry::ByteCount>::available(
        telemetry::ByteCount{network_transmit});
    return result;
}

} // namespace

static_assert(noexcept(telemetry::normalize_byte_rate(
    telemetry::MetricValue<telemetry::ByteCount>{}, telemetry::MetricValue<telemetry::ByteCount>{},
    std::chrono::steady_clock::duration{})));

TEST_CASE("the first observation establishes only cumulative baselines",
          "[telemetry][normalizer]") {
    telemetry::SystemTelemetryNormalizer normalizer;
    const auto raw = snapshot(std::chrono::steady_clock::time_point{10s}, 250U, 1000U, 16'000U,
                              10'000U, 100U, 200U, 300U, 400U);

    const auto result = normalizer.normalize(raw);

    CHECK(result.cpu_usage.status == telemetry::MetricStatus::temporarily_unavailable);
    CHECK(result.disk_read_rate.status == telemetry::MetricStatus::temporarily_unavailable);
    REQUIRE(result.memory_used.has_value());
    CHECK(result.memory_used.value.value == 6'000U);
    REQUIRE(result.memory_usage.has_value());
    CHECK(result.memory_usage.value.value == Approx(0.375));
}

TEST_CASE("opaque foreground identity and coarse pressure pass through normalization",
          "[telemetry][normalizer][privacy][pressure]") {
    telemetry::SystemTelemetryNormalizer normalizer;
    auto raw = snapshot(std::chrono::steady_clock::time_point{10s}, 250U, 1000U, 16'000U,
                        10'000U, 100U, 200U, 300U, 400U);
    raw.system.foreground_application =
        telemetry::MetricValue<telemetry::OpaqueApplicationIdentity>::available({19U, 23U});
    raw.system.memory_pressure_state =
        telemetry::MetricValue<telemetry::MemoryPressureState>::available(
            telemetry::MemoryPressureState::critical);

    const auto result = normalizer.normalize(raw);

    REQUIRE(result.foreground_application.has_value());
    CHECK(result.foreground_application.value.session_token == 19U);
    CHECK(result.foreground_application.value.application_token == 23U);
    REQUIRE(result.memory_pressure_state.has_value());
    CHECK(result.memory_pressure_state.value == telemetry::MemoryPressureState::critical);
}

TEST_CASE("memory activity normalizes independently and topology passes through",
          "[telemetry][normalizer][macos][memory]") {
    telemetry::SystemTelemetryNormalizer normalizer;
    auto first = snapshot(std::chrono::steady_clock::time_point{10s}, 100U, 1'000U, 8'000U,
                          4'000U, 100U, 100U, 100U, 100U);
    first.system.memory_activity.compressed_memory =
        telemetry::MetricValue<telemetry::ByteCount>::available({1'000U});
    first.system.memory_activity.page_out_bytes =
        telemetry::MetricValue<telemetry::ByteCount>::available({10'000U});
    first.system.memory_activity.swap_in_bytes =
        telemetry::MetricValue<telemetry::ByteCount>::available({20'000U});
    first.system.memory_activity.swap_out_bytes =
        telemetry::MetricValue<telemetry::ByteCount>::available({30'000U});
    first.system.memory_activity.compressed_bytes =
        telemetry::MetricValue<telemetry::ByteCount>::available({40'000U});
    first.system.memory_activity.decompressed_bytes =
        telemetry::MetricValue<telemetry::ByteCount>::available({50'000U});
    first.system.scheduler_delay = telemetry::MetricValue<telemetry::Seconds>::available({0.004});
    first.system.logical_processor_count =
        telemetry::MetricValue<std::uint32_t>::available(10U);
    first.system.physical_processor_count =
        telemetry::MetricValue<std::uint32_t>::available(8U);
    first.system.active_processor_count =
        telemetry::MetricValue<std::uint32_t>::available(6U);

    const auto warm = normalizer.normalize(first);
    CHECK(warm.memory_page_out_rate.status == telemetry::MetricStatus::temporarily_unavailable);
    REQUIRE(warm.compressed_memory.has_value());
    CHECK(warm.compressed_memory.value.value == 1'000U);
    REQUIRE(warm.scheduler_delay.has_value());
    CHECK(warm.scheduler_delay.value.value == Approx(0.004));
    CHECK(warm.logical_processor_count.value == 10U);
    CHECK(warm.physical_processor_count.value == 8U);
    CHECK(warm.active_processor_count.value == 6U);

    auto second = first;
    second.observed_at += 2s;
    second.system.memory_activity.page_out_bytes.value.value += 2'000U;
    second.system.memory_activity.swap_in_bytes.value.value += 4'000U;
    second.system.memory_activity.swap_out_bytes.value.value += 6'000U;
    second.system.memory_activity.compressed_bytes.value.value += 8'000U;
    second.system.memory_activity.decompressed_bytes.value.value += 10'000U;
    const auto result = normalizer.normalize(second);
    CHECK(result.memory_page_out_rate.value.value == Approx(1'000.0));
    CHECK(result.memory_swap_in_rate.value.value == Approx(2'000.0));
    CHECK(result.memory_swap_out_rate.value.value == Approx(3'000.0));
    CHECK(result.memory_compression_rate.value.value == Approx(4'000.0));
    CHECK(result.memory_decompression_rate.value.value == Approx(5'000.0));
}

TEST_CASE("cumulative counters normalize using measured elapsed time", "[telemetry][normalizer]") {
    telemetry::SystemTelemetryNormalizer normalizer;
    const auto first = snapshot(std::chrono::steady_clock::time_point{10s}, 100U, 1000U, 1000U,
                                400U, 100U, 200U, 300U, 400U);
    const auto second = snapshot(std::chrono::steady_clock::time_point{12s}, 500U, 1800U, 1000U,
                                 250U, 2'100U, 1'200U, 4'300U, 2'400U);

    static_cast<void>(normalizer.normalize(first));
    const auto result = normalizer.normalize(second);

    REQUIRE(result.cpu_usage.has_value());
    CHECK(result.cpu_usage.value.value == Approx(0.5));
    REQUIRE(result.disk_read_rate.has_value());
    CHECK(result.disk_read_rate.value.value == Approx(1000.0));
    CHECK(result.disk_write_rate.value.value == Approx(500.0));
    CHECK(result.network_receive_rate.value.value == Approx(2000.0));
    CHECK(result.network_transmit_rate.value.value == Approx(1000.0));
    CHECK(result.memory_usage.value.value == Approx(0.75));
}

TEST_CASE("zero and negative elapsed observations do not replace a valid baseline",
          "[telemetry][normalizer]") {
    telemetry::SystemTelemetryNormalizer normalizer;
    const auto baseline = snapshot(std::chrono::steady_clock::time_point{10s}, 100U, 1000U, 1000U,
                                   500U, 100U, 100U, 100U, 100U);
    const auto same_time = snapshot(std::chrono::steady_clock::time_point{10s}, 200U, 1200U, 1000U,
                                    500U, 200U, 200U, 200U, 200U);
    const auto older = snapshot(std::chrono::steady_clock::time_point{9s}, 250U, 1300U, 1000U, 500U,
                                250U, 250U, 250U, 250U);
    const auto valid = snapshot(std::chrono::steady_clock::time_point{11s}, 300U, 1400U, 1000U,
                                500U, 300U, 300U, 300U, 300U);

    static_cast<void>(normalizer.normalize(baseline));
    CHECK(normalizer.normalize(same_time).cpu_usage.status ==
          telemetry::MetricStatus::temporarily_unavailable);
    CHECK(normalizer.normalize(older).disk_read_rate.status ==
          telemetry::MetricStatus::temporarily_unavailable);

    const auto result = normalizer.normalize(valid);
    REQUIRE(result.cpu_usage.has_value());
    CHECK(result.cpu_usage.value.value == Approx(0.5));
    CHECK(result.disk_read_rate.value.value == Approx(200.0));
}

TEST_CASE("counter decreases are resets and never inferred wraps", "[telemetry][normalizer]") {
    const auto available = [](const std::uint64_t value) {
        return telemetry::MetricValue<telemetry::ByteCount>::available(telemetry::ByteCount{value});
    };

    const auto reset = telemetry::normalize_byte_rate(available(1000U), available(5U), 1s);
    const auto possible_wrap =
        telemetry::normalize_byte_rate(available(UINT64_MAX - 2U), available(3U), 1s);

    CHECK(reset.status == telemetry::MetricStatus::temporarily_unavailable);
    CHECK(possible_wrap.status == telemetry::MetricStatus::temporarily_unavailable);
}

TEST_CASE("cumulative stall durations normalize to bounded interval pressure",
          "[telemetry][normalizer][pressure]") {
    const auto value = [](const std::uint64_t microseconds) {
        return telemetry::MetricValue<std::uint64_t>::available(microseconds);
    };

    const auto quarter =
        telemetry::normalize_stall_fraction(value(1'000'000U), value(1'250'000U), 1s);
    REQUIRE(quarter.has_value());
    CHECK(quarter.value.value == Approx(0.25));

    const auto reset = telemetry::normalize_stall_fraction(value(1'000'000U), value(10U), 1s);
    CHECK(reset.status == telemetry::MetricStatus::temporarily_unavailable);
    const auto impossible = telemetry::normalize_stall_fraction(value(0U), value(1'000'010U), 1s);
    CHECK(impossible.status == telemetry::MetricStatus::temporarily_unavailable);
    const auto inaccessible = telemetry::normalize_stall_fraction(
        value(0U),
        telemetry::MetricValue<std::uint64_t>::unavailable(telemetry::MetricStatus::inaccessible),
        1s);
    CHECK(inaccessible.status == telemetry::MetricStatus::inaccessible);
}

TEST_CASE("system normalization warms and resets each pressure dimension "
          "independently",
          "[telemetry][normalizer][pressure][reset]") {
    telemetry::SystemTelemetryNormalizer normalizer;
    auto first = snapshot(std::chrono::steady_clock::time_point{10s}, 100U, 1'000U, 1'000U, 500U,
                          100U, 100U, 100U, 100U);
    first.system.pressure.cpu_some_microseconds =
        telemetry::MetricValue<std::uint64_t>::available(100U);
    first.system.pressure.memory_some_microseconds =
        telemetry::MetricValue<std::uint64_t>::available(200U);
    auto second = first;
    second.observed_at += 1s;
    second.system.pressure.cpu_some_microseconds.value = 100'100U;
    second.system.pressure.memory_some_microseconds.value = 50U;

    CHECK(normalizer.normalize(first).cpu_some_pressure.status ==
          telemetry::MetricStatus::temporarily_unavailable);
    const auto result = normalizer.normalize(second);
    REQUIRE(result.cpu_some_pressure.has_value());
    CHECK(result.cpu_some_pressure.value.value == Approx(0.1));
    CHECK(result.memory_some_pressure.status == telemetry::MetricStatus::temporarily_unavailable);
    CHECK(result.memory_full_pressure.status == telemetry::MetricStatus::unsupported);
}

TEST_CASE("normalization preserves current unavailable reasons", "[telemetry][normalizer]") {
    const auto previous =
        telemetry::MetricValue<telemetry::ByteCount>::available(telemetry::ByteCount{100U});
    const auto inaccessible = telemetry::MetricValue<telemetry::ByteCount>::unavailable(
        telemetry::MetricStatus::inaccessible);
    const auto unsupported = telemetry::MetricValue<telemetry::ByteCount>::unavailable(
        telemetry::MetricStatus::unsupported);

    CHECK(telemetry::normalize_byte_rate(previous, inaccessible, 1s).status ==
          telemetry::MetricStatus::inaccessible);
    CHECK(telemetry::normalize_byte_rate(previous, unsupported, 1s).status ==
          telemetry::MetricStatus::unsupported);
}

TEST_CASE("invalid memory gauges are temporarily unavailable", "[telemetry][normalizer]") {
    const auto total =
        telemetry::MetricValue<telemetry::ByteCount>::available(telemetry::ByteCount{100U});
    const auto too_much_available =
        telemetry::MetricValue<telemetry::ByteCount>::available(telemetry::ByteCount{101U});
    const auto zero =
        telemetry::MetricValue<telemetry::ByteCount>::available(telemetry::ByteCount{0U});

    CHECK(telemetry::normalize_memory_used(total, too_much_available).status ==
          telemetry::MetricStatus::temporarily_unavailable);
    CHECK(telemetry::normalize_memory_usage(zero, zero).status ==
          telemetry::MetricStatus::temporarily_unavailable);
}

TEST_CASE("TCP retransmission ratios require a useful interval population",
          "[telemetry][normalizer][network][robustness]") {
    const auto value = [](const std::uint64_t current) {
        return telemetry::MetricValue<std::uint64_t>::available(current);
    };

    const auto idle =
        telemetry::normalize_tcp_retransmit_fraction(value(10U), value(10U), value(2U), value(2U));
    REQUIRE(idle.has_value());
    CHECK(idle.value.value == Approx(0.0));

    const auto noisy =
        telemetry::normalize_tcp_retransmit_fraction(value(10U), value(15U), value(2U), value(3U));
    CHECK(noisy.status == telemetry::MetricStatus::temporarily_unavailable);

    const auto useful =
        telemetry::normalize_tcp_retransmit_fraction(value(10U), value(18U), value(2U), value(4U));
    REQUIRE(useful.has_value());
    CHECK(useful.value.value == Approx(0.2));

    const auto reset =
        telemetry::normalize_tcp_retransmit_fraction(value(10U), value(5U), value(2U), value(3U));
    CHECK(reset.status == telemetry::MetricStatus::temporarily_unavailable);
}

TEST_CASE("quality gauges and cumulative events normalize with explicit status",
          "[telemetry][normalizer][quality]") {
    telemetry::SystemTelemetryNormalizer normalizer;
    auto first = snapshot(std::chrono::steady_clock::time_point{10s}, 100U, 1'000U, 1'000U, 500U,
                          100U, 100U, 100U, 100U);
    first.system.disk_quality.service_time =
        telemetry::MetricValue<telemetry::Seconds>::available({0.125});
    first.system.disk_quality.queue_depth = telemetry::MetricValue<double>::available(9.0);
    first.system.disk_quality.service_concurrency = telemetry::MetricValue<double>::available(1.5);
    first.system.network_quality.connectivity =
        telemetry::MetricValue<telemetry::NetworkConnectivityLevel>::available(
            telemetry::NetworkConnectivityLevel::internet);
    first.system.network_quality.active_interfaces =
        telemetry::MetricValue<std::uint64_t>::available(1U);
    first.system.network_quality.interface_change_counter =
        telemetry::MetricValue<std::uint64_t>::available(20U);
    first.system.network_quality.tcp_out_segments =
        telemetry::MetricValue<std::uint64_t>::available(100U);
    first.system.network_quality.tcp_retransmitted_segments =
        telemetry::MetricValue<std::uint64_t>::available(10U);
    first.system.network_quality.tcp_failed_connections =
        telemetry::MetricValue<std::uint64_t>::available(2U);
    first.system.network_quality.tcp_established_resets =
        telemetry::MetricValue<std::uint64_t>::available(3U);

    const auto warmed = normalizer.normalize(first);
    REQUIRE(warmed.disk_service_time.has_value());
    CHECK(warmed.disk_service_time.value.value == Approx(0.125));
    REQUIRE(warmed.disk_queue_depth.has_value());
    CHECK(warmed.disk_queue_depth.value == Approx(9.0));
    REQUIRE(warmed.disk_service_concurrency.has_value());
    CHECK(warmed.disk_service_concurrency.value == Approx(1.5));
    CHECK(warmed.network_interface_changes.status ==
          telemetry::MetricStatus::temporarily_unavailable);

    auto second = first;
    second.observed_at += 1s;
    second.system.network_quality.connectivity.value =
        telemetry::NetworkConnectivityLevel::constrained;
    second.system.network_quality.interface_change_counter.value = 22U;
    second.system.network_quality.tcp_out_segments.value = 108U;
    second.system.network_quality.tcp_retransmitted_segments.value = 12U;
    second.system.network_quality.tcp_failed_connections.value = 4U;
    second.system.network_quality.tcp_established_resets.value = 4U;
    const auto result = normalizer.normalize(second);
    REQUIRE(result.network_connectivity.has_value());
    CHECK(result.network_connectivity.value == telemetry::NetworkConnectivityLevel::constrained);
    CHECK(result.network_interface_changes.value == 2U);
    REQUIRE(result.network_tcp_retransmit_fraction.has_value());
    CHECK(result.network_tcp_retransmit_fraction.value.value == Approx(0.2));
    CHECK(result.network_tcp_failed_connections.value == 2U);
    CHECK(result.network_tcp_resets.value == 1U);
}
