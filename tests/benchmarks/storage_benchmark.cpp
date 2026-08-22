#include "core/clock.hpp"
#include "storage/incident_archive.hpp"
#include "storage/incident_writer.hpp"
#include "telemetry/collector.hpp"
#include "telemetry/mock/mock_telemetry_provider.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace core = blackbox::core;
namespace storage = blackbox::storage;
namespace telemetry = blackbox::telemetry;
namespace mock = blackbox::telemetry::mock;
using namespace std::chrono_literals;

namespace {

class TemporaryArchive final {
public:
    TemporaryArchive() {
        static std::atomic<std::uint64_t> sequence{};
        path = std::filesystem::temp_directory_path() /
               ("blackbox-storage-benchmark-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(sequence.fetch_add(1U)) + ".sqlite3");
    }
    ~TemporaryArchive() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }
    std::filesystem::path path{};
};

[[nodiscard]] std::uint64_t working_set_bytes() noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) != FALSE) {
        return static_cast<std::uint64_t>(counters.WorkingSetSize);
    }
#endif
    return 0U;
}

[[nodiscard]] std::shared_ptr<const core::IncidentSnapshot> make_incident(
    const std::size_t process_count, const std::size_t frame_count) {
    core::IncidentHeader header{};
    header.window.sequence = 1U;
    header.window.event_time = core::MonotonicTimePoint{120s};
    header.window.requested_start = core::MonotonicTimePoint{0s};
    header.window.requested_end = core::MonotonicTimePoint{149s};
    header.actual_start = header.window.requested_start;
    header.actual_end = header.window.requested_end;

    std::vector<core::IncidentSystemSample> systems;
    std::vector<core::IncidentProcessInfo> metadata;
    std::vector<core::IncidentProcessSample> processes;
    systems.reserve(frame_count);
    metadata.reserve(process_count);
    processes.reserve(process_count * frame_count);
    for (std::size_t process = 0U; process < process_count; ++process) {
        core::IncidentProcessInfo info{};
        info.identity = {static_cast<std::uint32_t>(process + 1U), process + 100U};
        info.name = {"benchmark.exe", core::RecordedValueStatus::available};
        info.executable_path = {"C:\\Benchmark\\benchmark.exe",
                                core::RecordedValueStatus::available};
        metadata.push_back(std::move(info));
    }
    for (std::size_t frame = 0U; frame < frame_count; ++frame) {
        core::IncidentSystemSample system{};
        system.observed_at = core::MonotonicTimePoint{std::chrono::seconds{frame}};
        system.cpu_fraction = {0.5, core::RecordedValueStatus::available};
        system.memory_used_bytes = {8ULL << 30U, core::RecordedValueStatus::available};
        systems.push_back(system);
        for (const auto& info : metadata) {
            core::IncidentProcessSample sample{};
            sample.observed_at = system.observed_at;
            sample.identity = info.identity;
            sample.cpu_fraction = {0.01, core::RecordedValueStatus::available};
            sample.working_set_bytes = {64ULL << 20U,
                                        core::RecordedValueStatus::available};
            sample.disk_read_bytes_per_second = {1024.0,
                                                  core::RecordedValueStatus::available};
            sample.disk_write_bytes_per_second = {2048.0,
                                                   core::RecordedValueStatus::available};
            processes.push_back(sample);
        }
    }
    return std::make_shared<const core::IncidentSnapshot>(
        std::move(header), std::move(systems), std::move(metadata),
        std::move(processes));
}

void measure_store(const std::size_t process_count) {
    constexpr std::size_t frames = 150U;
    constexpr std::size_t trials = 5U;
    const auto incident = make_incident(process_count, frames);
    std::vector<double> elapsed_ms;
    elapsed_ms.reserve(trials);
    std::uint64_t database_bytes{};
    std::uint64_t maximum_temporary_bytes{};
    bool all_stored = true;
    for (std::size_t trial = 0U; trial < trials; ++trial) {
        TemporaryArchive temporary;
        storage::SqliteIncidentArchive archive{{temporary.path}};
        if (!archive.open()) return;
        const auto memory_before = working_set_bytes();
        std::atomic<bool> monitoring{true};
        std::atomic<std::uint64_t> peak{memory_before};
        std::jthread monitor{[&](const std::stop_token stop_token) {
            while (!stop_token.stop_requested() && monitoring.load()) {
                auto observed = working_set_bytes();
                auto previous = peak.load();
                while (observed > previous &&
                       !peak.compare_exchange_weak(previous, observed)) {}
                std::this_thread::sleep_for(250us);
            }
        }};
        const auto started = std::chrono::steady_clock::now();
        const auto stored = archive.store(*incident);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        monitoring.store(false);
        monitor.request_stop();
        monitor.join();
        const auto memory_after = working_set_bytes();
        peak.store(std::max(peak.load(), memory_after));
        elapsed_ms.push_back(
            std::chrono::duration<double, std::milli>{elapsed}.count());
        const auto size = archive.database_size_bytes();
        database_bytes = size ? *size : 0U;
        maximum_temporary_bytes = std::max(
            maximum_temporary_bytes,
            peak.load() > memory_before ? peak.load() - memory_before : 0U);
        all_stored = all_stored && stored.has_value();
    }
    std::sort(elapsed_ms.begin(), elapsed_ms.end());
    const auto average = std::accumulate(elapsed_ms.begin(), elapsed_ms.end(), 0.0) /
                         static_cast<double>(elapsed_ms.size());
    std::cout << process_count << ',' << process_count * frames << ','
              << average << ',' << elapsed_ms.back() << ',' << elapsed_ms.back() << ','
              << elapsed_ms.back() << ',' << database_bytes << ','
              << maximum_temporary_bytes << ',' << static_cast<int>(all_stored) << '\n';
}

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate,
                              const std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

void measure_async_jitter() {
    TemporaryArchive temporary;
    storage::SqliteIncidentArchive archive{{temporary.path}};
    if (!archive.open()) return;
    core::SystemMonotonicClock clock;
    mock::MockTelemetryProvider provider{clock};
    auto values = telemetry::RecorderConfiguration{2ms, 100ms, 1ms};
    values.incident_pre_window = 50ms;
    values.incident_post_window = 20ms;
    const auto configuration = telemetry::validate_recorder_configuration(values);
    if (!configuration) return;
    telemetry::TelemetryCollector collector{provider, clock, *configuration};
    storage::IncidentWriter writer{collector.incident_work_source(), archive};
    writer.start();
    collector.start();
    static_cast<void>(wait_until(
        [&collector] { return collector.diagnostics().collection_count >= 30U; }));
    const auto database_before_capture = archive.database_size_bytes();
    static_cast<void>(collector.request_incident_capture());
    static_cast<void>(wait_until([&writer] {
        return writer.diagnostics().succeeded == 1U;
    }));
    static_cast<void>(wait_until(
        [&collector] { return collector.diagnostics().collection_count >= 60U; }));
    collector.stop();
    writer.stop();
    const auto collector_diagnostics = collector.diagnostics();
    const auto writer_diagnostics = writer.diagnostics();
    const auto database_after_capture = archive.database_size_bytes();
    std::cout << "async_capture,writer_p99_ms="
              << std::chrono::duration<double, std::milli>{
                     writer_diagnostics.write_timing.p99}.count()
              << ",jitter_p99_us="
              << std::chrono::duration<double, std::micro>{
                     collector_diagnostics.scheduling_jitter.p99}.count()
              << ",deadline_misses=" << collector_diagnostics.deadline_misses
              << ",dropped=" << collector_diagnostics.dropped_samples
              << ",bytes_before_capture="
              << (database_before_capture ? *database_before_capture : 0U)
              << ",bytes_after_capture="
              << (database_after_capture ? *database_after_capture : 0U) << '\n';
}

} // namespace

int main() {
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "processes,process_samples,average_ms,p95_ms,p99_ms,maximum_ms,"
                 "database_bytes,peak_temporary_working_set_bytes,all_stored\n";
    measure_store(50U);
    measure_store(200U);
    measure_store(500U);
    measure_async_jitter();
    return 0;
}
