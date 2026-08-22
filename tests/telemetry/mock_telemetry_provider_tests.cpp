#include "telemetry/mock/mock_telemetry_provider.hpp"
#include "telemetry/normalizer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>

namespace core = blackbox::core;
namespace telemetry = blackbox::telemetry;
namespace mock = blackbox::telemetry::mock;
using Catch::Approx;
using namespace std::chrono_literals;

namespace {

class ManualClock final : public core::IMonotonicClock {
public:
    [[nodiscard]] core::MonotonicTimePoint now() const noexcept override {
        return now_;
    }

    void advance(const core::MonotonicClock::duration amount) noexcept {
        now_ += amount;
    }

private:
    core::MonotonicTimePoint now_{};
};

telemetry::SystemSample sample_at_spike(
    const mock::Scenario scenario,
    telemetry::RawTelemetrySnapshot* raw_at_spike = nullptr,
    telemetry::RawTelemetrySnapshot* raw_before_spike = nullptr) {
    ManualClock clock;
    mock::MockTelemetryProvider provider{clock, scenario};
    telemetry::SystemTelemetryNormalizer normalizer;
    telemetry::RawTelemetrySnapshot raw;
    telemetry::SystemSample normalized;

    for (std::size_t index = 0; index <= 4U; ++index) {
        if (index == 3U && raw_before_spike != nullptr) {
            static_cast<void>(provider.sample({}, *raw_before_spike));
            normalized = normalizer.normalize(*raw_before_spike);
        } else {
            static_cast<void>(provider.sample({}, raw));
            normalized = normalizer.normalize(raw);
            if (index == 4U && raw_at_spike != nullptr) {
                *raw_at_spike = raw;
            }
        }
        clock.advance(1s);
    }
    return normalized;
}
} // namespace

TEST_CASE("normal mock output is deterministic and normalized", "[telemetry][mock]") {
    ManualClock first_clock;
    ManualClock second_clock;
    mock::MockTelemetryProvider first{first_clock};
    mock::MockTelemetryProvider second{second_clock};
    telemetry::RawTelemetrySnapshot first_raw;
    telemetry::RawTelemetrySnapshot second_raw;

    for (int index = 0; index < 6; ++index) {
        CHECK(first.sample({}, first_raw) == second.sample({}, second_raw));
        CHECK(first_raw == second_raw);
        first_clock.advance(1s);
        second_clock.advance(1s);
    }

    const auto normalized = sample_at_spike(mock::Scenario::normal);
    CHECK(normalized.cpu_usage.value.value == Approx(0.25));
    CHECK(normalized.disk_read_rate.value.value == Approx(1024.0 * 1024.0));
    CHECK(normalized.network_receive_rate.value.value == Approx(128.0 * 1024.0));
}

TEST_CASE("mock spike scenarios change only their intended signals", "[telemetry][mock]") {
    const auto cpu = sample_at_spike(mock::Scenario::cpu_spike);
    const auto disk = sample_at_spike(mock::Scenario::disk_spike);
    const auto network = sample_at_spike(mock::Scenario::network_drop);

    CHECK(cpu.cpu_usage.value.value == Approx(0.9));
    CHECK(cpu.disk_read_rate.value.value == Approx(1024.0 * 1024.0));
    CHECK(disk.cpu_usage.value.value == Approx(0.25));
    CHECK(disk.disk_read_rate.value.value == Approx(64.0 * 1024.0 * 1024.0));
    CHECK(disk.disk_write_rate.value.value == Approx(32.0 * 1024.0 * 1024.0));
    CHECK(network.network_receive_rate.value.value == Approx(0.0));
    CHECK(network.network_transmit_rate.value.value == Approx(0.0));
}

TEST_CASE("process spike scenario has stable identity and cumulative changes",
          "[telemetry][mock]") {
    telemetry::RawTelemetrySnapshot before;
    telemetry::RawTelemetrySnapshot spike;
    static_cast<void>(sample_at_spike(mock::Scenario::process_spike, &spike, &before));

    REQUIRE(before.processes.size() == 1U);
    REQUIRE(spike.processes.size() == 1U);
    CHECK(before.processes.front().identity == spike.processes.front().identity);
    CHECK(spike.processes.front().cpu_time.value - before.processes.front().cpu_time.value == 800ms);
    CHECK(spike.processes.front().disk_read_bytes.value.value -
          before.processes.front().disk_read_bytes.value.value == 8U * 1024U * 1024U);
}

TEST_CASE("sampling requests select tiers without implying a scheduler",
          "[telemetry][mock]") {
    ManualClock clock;
    mock::MockTelemetryProvider provider{clock};
    telemetry::RawTelemetrySnapshot raw;

    const telemetry::SamplingRequest fast_only{telemetry::SamplingTier::fast};
    static_cast<void>(provider.sample(fast_only, raw));
    CHECK(raw.system.cpu_time.has_value());
    CHECK(raw.system.memory_total.status == telemetry::MetricStatus::temporarily_unavailable);
    CHECK(raw.processes.empty());
    CHECK(raw.process_metadata.empty());

    const telemetry::SamplingRequest slow_only{telemetry::SamplingTier::slow};
    static_cast<void>(provider.sample(slow_only, raw));
    CHECK(raw.system.cpu_time.status == telemetry::MetricStatus::temporarily_unavailable);
    CHECK(raw.processes.empty());
    REQUIRE(raw.process_metadata.size() == 1U);
    CHECK(raw.process_metadata.front().name.value == "mock-worker");
}

TEST_CASE("capability loss and recovery remain explicit and safe",
          "[telemetry][mock]") {
    ManualClock clock;
    mock::MockTelemetryProvider provider{clock};
    telemetry::SystemTelemetryNormalizer normalizer;
    telemetry::RawTelemetrySnapshot raw;

    static_cast<void>(provider.sample({}, raw));
    static_cast<void>(normalizer.normalize(raw));

    auto capabilities = provider.capabilities();
    capabilities.network_usage = false;
    provider.set_capabilities(capabilities);
    clock.advance(1s);
    static_cast<void>(provider.sample({}, raw));
    const auto lost = normalizer.normalize(raw);
    CHECK(lost.network_receive_rate.status == telemetry::MetricStatus::unsupported);

    capabilities.network_usage = true;
    provider.set_capabilities(capabilities);
    clock.advance(1s);
    static_cast<void>(provider.sample({}, raw));
    const auto recovered_baseline = normalizer.normalize(raw);
    CHECK(recovered_baseline.network_receive_rate.status ==
          telemetry::MetricStatus::temporarily_unavailable);

    clock.advance(1s);
    static_cast<void>(provider.sample({}, raw));
    const auto recovered = normalizer.normalize(raw);
    REQUIRE(recovered.network_receive_rate.has_value());
    CHECK(recovered.network_receive_rate.value.value == Approx(128.0 * 1024.0));
}
