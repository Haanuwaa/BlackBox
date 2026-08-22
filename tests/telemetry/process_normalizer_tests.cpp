#include "telemetry/process_normalizer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

namespace telemetry = blackbox::telemetry;
using Catch::Approx;
using namespace std::chrono_literals;

namespace {

telemetry::RawTelemetrySnapshot snapshot(
    const std::chrono::steady_clock::time_point time,
    const telemetry::ProcessIdentity identity,
    const std::chrono::nanoseconds cpu,
    const std::uint64_t read,
    const std::uint64_t write) {
    telemetry::RawTelemetrySnapshot result;
    result.reset(time, telemetry::SamplingTier::normal);
    result.system.logical_processor_count =
        telemetry::MetricValue<std::uint32_t>::available(8U);
    telemetry::RawProcessCounters process{};
    process.identity = identity;
    process.cpu_time = telemetry::MetricValue<std::chrono::nanoseconds>::available(cpu);
    process.working_set = telemetry::MetricValue<telemetry::ByteCount>::available(
        telemetry::ByteCount{64U * 1024U * 1024U});
    process.disk_read_bytes = telemetry::MetricValue<telemetry::ByteCount>::available(
        telemetry::ByteCount{read});
    process.disk_write_bytes = telemetry::MetricValue<telemetry::ByteCount>::available(
        telemetry::ByteCount{write});
    result.processes.push_back(process);
    return result;
}

} // namespace

TEST_CASE("process counters normalize to total-machine CPU and byte rates",
          "[telemetry][process][normalizer]") {
    const telemetry::ProcessIdentity identity{telemetry::ProcessId{42U}, 100U};
    telemetry::ProcessTelemetryNormalizer normalizer;
    std::vector<telemetry::ProcessSample> output;
    normalizer.normalize(snapshot(std::chrono::steady_clock::time_point{1s},
                                  identity, 1s, 100U, 200U), output);
    REQUIRE(output.size() == 1U);
    CHECK(output[0].cpu_usage.status == telemetry::MetricStatus::temporarily_unavailable);

    normalizer.normalize(snapshot(std::chrono::steady_clock::time_point{2s},
                                  identity, 1800ms, 1'100U, 700U), output);
    REQUIRE(output.size() == 1U);
    REQUIRE(output[0].cpu_usage.has_value());
    CHECK(output[0].cpu_usage.value.value == Approx(0.1));
    CHECK(output[0].disk_read_rate.value.value == Approx(1'000.0));
    CHECK(output[0].disk_write_rate.value.value == Approx(500.0));
    CHECK(output[0].working_set.value.value == 64U * 1024U * 1024U);
}

TEST_CASE("PID reuse and process reappearance always establish new baselines",
          "[telemetry][process][normalizer]") {
    telemetry::ProcessTelemetryNormalizer normalizer;
    std::vector<telemetry::ProcessSample> output;
    const telemetry::ProcessIdentity first{telemetry::ProcessId{77U}, 1U};
    const telemetry::ProcessIdentity reused{telemetry::ProcessId{77U}, 2U};

    normalizer.normalize(snapshot(std::chrono::steady_clock::time_point{1s},
                                  first, 10s, 10'000U, 10'000U), output);
    normalizer.normalize(snapshot(std::chrono::steady_clock::time_point{2s},
                                  reused, 100ms, 5U, 5U), output);
    REQUIRE(output.size() == 1U);
    CHECK(output[0].identity == reused);
    CHECK(output[0].cpu_usage.status == telemetry::MetricStatus::temporarily_unavailable);
    CHECK(normalizer.tracked_processes() == 1U);

    telemetry::RawTelemetrySnapshot empty;
    empty.reset(std::chrono::steady_clock::time_point{3s},
                telemetry::SamplingTier::normal);
    empty.system.logical_processor_count =
        telemetry::MetricValue<std::uint32_t>::available(8U);
    normalizer.normalize(empty, output);
    CHECK(output.empty());
    CHECK(normalizer.tracked_processes() == 0U);

    normalizer.normalize(snapshot(std::chrono::steady_clock::time_point{4s},
                                  reused, 1s, 500U, 500U), output);
    REQUIRE(output.size() == 1U);
    CHECK(output[0].disk_read_rate.status ==
          telemetry::MetricStatus::temporarily_unavailable);
}

TEST_CASE("process normalization preserves access failures and rejects invalid CPU",
          "[telemetry][process][normalizer]") {
    const auto available_cpu = telemetry::MetricValue<std::chrono::nanoseconds>::available;
    const auto processors = telemetry::MetricValue<std::uint32_t>::available(4U);
    CHECK(telemetry::normalize_process_cpu(
              available_cpu(2s), available_cpu(1s), 1s, processors).status ==
          telemetry::MetricStatus::temporarily_unavailable);
    CHECK(telemetry::normalize_process_cpu(
              available_cpu(0s), available_cpu(10s), 1s, processors).status ==
          telemetry::MetricStatus::temporarily_unavailable);

    const auto inaccessible =
        telemetry::MetricValue<std::chrono::nanoseconds>::unavailable(
            telemetry::MetricStatus::inaccessible);
    CHECK(telemetry::normalize_process_cpu(
              available_cpu(0s), inaccessible, 1s, processors).status ==
          telemetry::MetricStatus::inaccessible);
}
