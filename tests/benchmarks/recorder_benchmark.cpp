#include "core/circular_recorder.hpp"
#include "core/clock.hpp"
#include "telemetry/collection_timing.hpp"
#include "telemetry/collector.hpp"
#include "telemetry/mock/mock_telemetry_provider.hpp"
#include "telemetry/types.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>

namespace core = blackbox::core;
namespace telemetry = blackbox::telemetry;
namespace mock = blackbox::telemetry::mock;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] std::uint64_t argument_or(const int argc,
                                        char** argv,
                                        const int index,
                                        const std::uint64_t fallback) noexcept {
    if (index >= argc) {
        return fallback;
    }
    const std::string_view text{argv[index]};
    std::uint64_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return fallback;
    }
    return value;
}

[[nodiscard]] double microseconds(const std::chrono::nanoseconds duration) noexcept {
    return std::chrono::duration<double, std::micro>{duration}.count();
}

void print_timing(const std::string_view name,
                  const telemetry::CollectionTimingSummary& summary) {
    std::cout << name << "_samples=" << summary.samples_recorded << '\n'
              << name << "_average_us=" << microseconds(summary.average) << '\n'
              << name << "_p50_us=" << microseconds(summary.p50) << '\n'
              << name << "_p95_us=" << microseconds(summary.p95) << '\n'
              << name << "_p99_us=" << microseconds(summary.p99) << '\n'
              << name << "_maximum_us=" << microseconds(summary.maximum) << '\n';
}

} // namespace

int main(const int argc, char** argv) {
    const auto operation_count = argument_or(argc, argv, 1, 1'000'000U);
    const auto live_duration_ms = argument_or(argc, argv, 2, 2'000U);
    const auto live_interval_ms = argument_or(argc, argv, 3, 1U);

    core::CircularRecorder<telemetry::SystemSample> recorder{300U};
    telemetry::SystemSample sample{};
    sample.cpu_usage = telemetry::MetricValue<telemetry::Ratio>::available(
        telemetry::Ratio{0.5});
    sample.memory_usage = telemetry::MetricValue<telemetry::Ratio>::available(
        telemetry::Ratio{0.4});

    telemetry::CollectionTimingWindow append_timing;
    for (std::uint64_t index = 0U; index < operation_count; ++index) {
        sample.observed_at = core::MonotonicTimePoint{
            std::chrono::nanoseconds{static_cast<std::int64_t>(index)}};
        const auto started = std::chrono::steady_clock::now();
        recorder.append(sample);
        const auto finished = std::chrono::steady_clock::now();
        append_timing.record(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started));
    }

    telemetry::CollectionTimingWindow snapshot_timing;
    std::uint64_t snapshot_checksum{};
    const auto snapshot_operations = operation_count / 100U + 1U;
    for (std::uint64_t index = 0U; index < snapshot_operations; ++index) {
        const auto started = std::chrono::steady_clock::now();
        const auto snapshot = recorder.snapshot(300U);
        const auto finished = std::chrono::steady_clock::now();
        snapshot_checksum += snapshot.size();
        snapshot_timing.record(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started));
    }

    core::SystemMonotonicClock clock;
    mock::MockTelemetryProvider provider{clock};
    const telemetry::RecorderConfiguration requested{
        std::chrono::milliseconds{live_interval_ms},
        std::chrono::milliseconds{live_interval_ms * 300U},
        std::chrono::milliseconds{live_interval_ms / 20U}};
    const auto validated = telemetry::validate_recorder_configuration(requested);
    if (!validated) {
        std::cerr << "invalid live collector configuration\n";
        return 2;
    }

    telemetry::TelemetryCollector collector{provider, clock, *validated};
    collector.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{live_duration_ms});
    collector.stop();
    const auto diagnostics = collector.diagnostics();

    std::cout << "operations=" << operation_count << '\n'
              << "system_sample_bytes=" << sizeof(telemetry::SystemSample) << '\n'
              << "default_ring_payload_bytes="
              << sizeof(telemetry::SystemSample) * 300U << '\n'
              << "snapshot_checksum=" << snapshot_checksum << '\n';
    print_timing("append", append_timing.summary());
    print_timing("snapshot_300", snapshot_timing.summary());
    std::cout << "live_interval_ms=" << live_interval_ms << '\n'
              << "live_duration_ms=" << live_duration_ms << '\n'
              << "live_collections=" << diagnostics.collection_count << '\n'
              << "live_dropped=" << diagnostics.dropped_samples << '\n'
              << "live_late=" << diagnostics.late_samples << '\n'
              << "live_deadline_misses=" << diagnostics.deadline_misses << '\n'
              << "live_ring_size=" << diagnostics.ring.size << '\n'
              << "live_ring_capacity=" << diagnostics.ring.capacity << '\n'
              << "live_ring_overwrites=" << diagnostics.ring.overwritten_samples << '\n';
    print_timing("live_collection", diagnostics.collection_timing);
    print_timing("live_jitter", diagnostics.scheduling_jitter);
    return 0;
}
