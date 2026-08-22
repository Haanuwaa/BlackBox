#include "core/circular_recorder.hpp"
#include "core/clock.hpp"
#include "telemetry/collector.hpp"
#include "telemetry/incident_snapshot_builder.hpp"
#include "telemetry/mock/mock_telemetry_provider.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace core = blackbox::core;
namespace telemetry = blackbox::telemetry;
namespace mock = blackbox::telemetry::mock;
using namespace std::chrono_literals;

namespace {

struct TimingSummary {
    double average_ms{};
    double p95_ms{};
    double p99_ms{};
    double maximum_ms{};
};

[[nodiscard]] TimingSummary summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    double total = 0.0;
    for (const auto value : values) {
        total += value;
    }
    const auto percentile = [&values](const double fraction) {
        const auto rank = static_cast<std::size_t>(
            std::ceil(fraction * static_cast<double>(values.size())));
        return values[std::min(values.size() - 1U, std::max<std::size_t>(1U, rank) - 1U)];
    };
    return {total / static_cast<double>(values.size()), percentile(0.95),
            percentile(0.99), values.back()};
}

[[nodiscard]] telemetry::SystemSample system_sample(const std::size_t tick) {
    telemetry::SystemSample sample{};
    sample.observed_at = core::MonotonicTimePoint{std::chrono::seconds{tick}};
    sample.cpu_usage = telemetry::MetricValue<telemetry::Ratio>::available({0.5});
    sample.memory_used = telemetry::MetricValue<telemetry::ByteCount>::available({1U});
    sample.memory_total = telemetry::MetricValue<telemetry::ByteCount>::available({2U});
    return sample;
}

void measure_snapshot_scale(const std::size_t process_count) {
    constexpr std::size_t frame_count = 150U;
    std::vector<telemetry::SystemSample> systems;
    std::vector<telemetry::ProcessFrame> frames;
    std::vector<telemetry::ProcessInfo> metadata;
    systems.reserve(frame_count);
    frames.reserve(frame_count);
    metadata.reserve(process_count);

    for (std::size_t process = 0U; process < process_count; ++process) {
        telemetry::ProcessInfo info{};
        info.identity = {{static_cast<std::uint32_t>(process + 1U)}, process + 100U};
        info.name = telemetry::MetricValue<std::string>::available("benchmark-process");
        metadata.push_back(std::move(info));
    }
    for (std::size_t tick = 0U; tick < frame_count; ++tick) {
        systems.push_back(system_sample(tick));
        telemetry::ProcessFrame frame{};
        frame.observed_at = core::MonotonicTimePoint{std::chrono::seconds{tick}};
        frame.processes.reserve(process_count);
        for (std::size_t process = 0U; process < process_count; ++process) {
            telemetry::ProcessSample sample{};
            sample.identity = metadata[process].identity;
            sample.cpu_usage = telemetry::MetricValue<telemetry::Ratio>::available({0.01});
            sample.working_set = telemetry::MetricValue<telemetry::ByteCount>::available({1U});
            sample.disk_read_rate =
                telemetry::MetricValue<telemetry::BytesPerSecond>::available({2.0});
            sample.disk_write_rate =
                telemetry::MetricValue<telemetry::BytesPerSecond>::available({3.0});
            frame.processes.push_back(sample);
        }
        frames.push_back(std::move(frame));
    }

    const core::RecorderSnapshot<telemetry::SystemSample> system_history{0U,
                                                                         systems};
    const core::RecorderSnapshot<telemetry::ProcessFrame> process_history{0U,
                                                                           frames};
    const core::IncidentCaptureWindow window{
        1U, core::MonotonicTimePoint{120s}, core::MonotonicTimePoint{0s},
        core::MonotonicTimePoint{149s}, 1U};
    std::vector<double> timings;
    timings.reserve(10U);
    std::size_t checksum = 0U;
    for (std::size_t iteration = 0U; iteration < 10U; ++iteration) {
        const auto started = std::chrono::steady_clock::now();
        const auto incident = telemetry::build_incident_snapshot(
            window, core::MonotonicTimePoint{149s}, system_history,
            process_history, metadata);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        timings.push_back(std::chrono::duration<double, std::milli>{elapsed}.count());
        checksum += incident->process_samples().size();
    }
    const auto timing = summarize(std::move(timings));
    std::cout << process_count << ',' << frame_count << ','
              << process_count * frame_count << ',' << timing.average_ms << ','
              << timing.p95_ms << ',' << timing.p99_ms << ',' << timing.maximum_ms
              << ',' << checksum << '\n';
}

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate,
                              const std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

void measure_collector_capture_jitter() {
    core::SystemMonotonicClock clock;
    mock::MockTelemetryProvider provider{clock};
    auto values = telemetry::RecorderConfiguration{2ms, 100ms, 1ms};
    values.incident_pre_window = 50ms;
    values.incident_post_window = 20ms;
    const auto configuration = telemetry::validate_recorder_configuration(values);
    if (!configuration.has_value()) {
        return;
    }
    telemetry::TelemetryCollector collector{provider, clock, *configuration};
    collector.start();
    static_cast<void>(wait_until(
        [&collector] { return collector.diagnostics().collection_count >= 30U; }));
    static_cast<void>(collector.request_incident_capture());
    static_cast<void>(wait_until([&collector] {
        return collector.incident_capture_status().incidents_completed == 1U;
    }));
    collector.stop();
    const auto diagnostics = collector.diagnostics();
    std::cout << "collector_capture,jitter_p95_us="
              << std::chrono::duration<double, std::micro>{
                     diagnostics.scheduling_jitter.p95}.count()
              << ",jitter_p99_us="
              << std::chrono::duration<double, std::micro>{
                     diagnostics.scheduling_jitter.p99}.count()
              << ",snapshot_us="
              << std::chrono::duration<double, std::micro>{
                     diagnostics.incident_snapshot_timing.maximum}.count()
              << ",deadline_misses=" << diagnostics.deadline_misses
              << ",dropped=" << diagnostics.dropped_samples << '\n';
}

} // namespace

int main() {
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "sizeof_incident_system_sample="
              << sizeof(core::IncidentSystemSample)
              << ",sizeof_incident_process_sample="
              << sizeof(core::IncidentProcessSample) << '\n';
    std::cout << "processes,frames,process_samples,average_ms,p95_ms,p99_ms,maximum_ms,checksum\n";
    measure_snapshot_scale(50U);
    measure_snapshot_scale(200U);
    measure_snapshot_scale(500U);
    measure_collector_capture_jitter();
    return 0;
}
