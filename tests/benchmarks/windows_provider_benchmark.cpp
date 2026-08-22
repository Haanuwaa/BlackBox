#include "core/clock.hpp"
#include "telemetry/collection_timing.hpp"
#include "telemetry/collector.hpp"
#include "telemetry/normalizer.hpp"
#include "telemetry/windows/windows_telemetry_provider.hpp"

#include <charconv>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

namespace {

[[nodiscard]] std::uint64_t parse_or(
    const char* text,
    const std::uint64_t fallback) noexcept {
    if (text == nullptr) {
        return fallback;
    }
    const std::string_view input{text};
    std::uint64_t value{};
    const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
    return result.ec == std::errc{} && result.ptr == input.data() + input.size()
               ? value
               : fallback;
}

[[nodiscard]] double nanoseconds(
    const std::chrono::nanoseconds duration) noexcept {
    return static_cast<double>(duration.count());
}

[[nodiscard]] std::uint64_t file_time_value(const FILETIME value) noexcept {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
           static_cast<std::uint64_t>(value.dwLowDateTime);
}

[[nodiscard]] bool process_is_elevated() noexcept {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned{};
    const auto succeeded = GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &returned);
    CloseHandle(token);
    return succeeded && elevation.TokenIsElevated != 0U;
}

} // namespace

int main(int argc, char** argv) {
    namespace core = blackbox::core;
    namespace telemetry = blackbox::telemetry;
    namespace windows = blackbox::telemetry::windows;

    const auto sample_count = argc > 1 ? parse_or(argv[1], 100'000U) : 100'000U;
    const auto interval_ms = argc > 2 ? parse_or(argv[2], 0U) : 0U;
    const auto collector_seconds = argc > 3 ? parse_or(argv[3], 0U) : 0U;

    core::SystemMonotonicClock clock;
    auto provider = std::make_unique<windows::WindowsTelemetryProvider>(clock);
    telemetry::SystemTelemetryNormalizer normalizer;
    telemetry::CollectionTimingWindow timing;
    telemetry::RawTelemetrySnapshot raw;
    double checksum = 0.0;
    std::uint64_t disk_samples = 0U;
    std::uint64_t network_samples = 0U;

    for (std::uint64_t index = 0U; index < sample_count; ++index) {
        const auto started = clock.now();
        const auto result = provider->sample({}, raw);
        const auto finished = clock.now();
        timing.record(std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started));

        const auto sample = normalizer.normalize(raw);
        if (sample.cpu_usage.has_value()) {
            checksum += sample.cpu_usage.value.value;
            if (interval_ms != 0U) {
                std::fprintf(stderr, "cpu_percent[%llu]=%.3f\n",
                            static_cast<unsigned long long>(index),
                            sample.cpu_usage.value.value * 100.0);
            }
        }
        if (sample.memory_used.has_value()) {
            checksum += static_cast<double>(sample.memory_used.value.value % 1'000'003U);
        }
        if (sample.disk_read_rate.has_value() && sample.disk_write_rate.has_value()) {
            ++disk_samples;
            checksum += sample.disk_read_rate.value.value;
            checksum += sample.disk_write_rate.value.value;
            if (interval_ms != 0U) {
                std::fprintf(stderr, "disk_mib_per_second[%llu]=%.3f/%.3f\n",
                            static_cast<unsigned long long>(index),
                            sample.disk_read_rate.value.value / (1024.0 * 1024.0),
                            sample.disk_write_rate.value.value / (1024.0 * 1024.0));
            }
        }
        if (sample.network_receive_rate.has_value() &&
            sample.network_transmit_rate.has_value()) {
            ++network_samples;
            checksum += sample.network_receive_rate.value.value;
            checksum += sample.network_transmit_rate.value.value;
            if (interval_ms != 0U) {
                std::fprintf(stderr, "network_mib_per_second[%llu]=%.3f/%.3f\n",
                            static_cast<unsigned long long>(index),
                            sample.network_receive_rate.value.value / (1024.0 * 1024.0),
                            sample.network_transmit_rate.value.value / (1024.0 * 1024.0));
            }
        }
        if (result.status != telemetry::ProviderSampleStatus::complete) {
            std::fprintf(stderr, "provider_status[%llu]=%d\n",
                        static_cast<unsigned long long>(index),
                        static_cast<int>(result.status));
        }

        if (interval_ms != 0U && index + 1U < sample_count) {
            std::this_thread::sleep_for(std::chrono::milliseconds{interval_ms});
        }
    }

    const auto summary = timing.summary();
    std::fprintf(stderr, "samples=%llu\nwindow_samples=%zu\naverage_ns=%.0f\n"
                "p95_ns=%.0f\np99_ns=%.0f\nmaximum_ns=%.0f\n"
                "deadline_margin_percent=%.6f\ndisk_rate_samples=%llu\n"
                "network_rate_samples=%llu\nchecksum=%.3f\n",
                static_cast<unsigned long long>(summary.samples_recorded),
                summary.samples_in_window, nanoseconds(summary.average),
                nanoseconds(summary.p95), nanoseconds(summary.p99),
                nanoseconds(summary.maximum),
                100.0 - nanoseconds(summary.p99) / 10'000'000.0,
                static_cast<unsigned long long>(disk_samples),
                static_cast<unsigned long long>(network_samples), checksum);

    if (collector_seconds != 0U) {
        auto collector_provider =
            std::make_unique<windows::WindowsTelemetryProvider>(clock);
        const auto configuration = telemetry::validate_recorder_configuration({});
        if (!configuration) {
            return 2;
        }
        auto collector = std::make_unique<telemetry::TelemetryCollector>(
            *collector_provider, clock, *configuration);
        FILETIME created{}, exited{}, kernel_before{}, user_before{};
        IO_COUNTERS io_before{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited,
                        &kernel_before, &user_before);
        GetProcessIoCounters(GetCurrentProcess(), &io_before);
        std::uint64_t working_set_total{};
        std::uint64_t private_total{};
        std::uint64_t working_set_maximum{};
        std::uint64_t private_maximum{};
        std::uint64_t memory_observations{};
        collector->start();
        const auto wall_started = std::chrono::steady_clock::now();
        const auto deadline = wall_started + std::chrono::seconds{collector_seconds};
        while (std::chrono::steady_clock::now() < deadline) {
            PROCESS_MEMORY_COUNTERS_EX memory{};
            memory.cb = sizeof(memory);
            if (GetProcessMemoryInfo(
                    GetCurrentProcess(),
                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                    sizeof(memory))) {
                working_set_total += memory.WorkingSetSize;
                private_total += memory.PrivateUsage;
                working_set_maximum = (std::max)(
                    working_set_maximum,
                    static_cast<std::uint64_t>(memory.WorkingSetSize));
                private_maximum = (std::max)(
                    private_maximum,
                    static_cast<std::uint64_t>(memory.PrivateUsage));
                ++memory_observations;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        collector->stop();
        const auto wall_finished = std::chrono::steady_clock::now();
        FILETIME kernel_after{}, user_after{};
        IO_COUNTERS io_after{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited,
                        &kernel_after, &user_after);
        GetProcessIoCounters(GetCurrentProcess(), &io_after);
        const auto diagnostics = collector->diagnostics();
        const auto process_100ns =
            file_time_value(kernel_after) - file_time_value(kernel_before) +
            file_time_value(user_after) - file_time_value(user_before);
        const auto wall_seconds = std::chrono::duration<double>(
            wall_finished - wall_started).count();
        const auto processors = (std::max)(1U, std::thread::hardware_concurrency());
        const auto cpu_percent =
            static_cast<double>(process_100ns) / (wall_seconds * 10'000'000.0) /
            static_cast<double>(processors) * 100.0;
        const auto working_set_average = memory_observations == 0U
                                             ? 0U
                                             : working_set_total / memory_observations;
        const auto private_average = memory_observations == 0U
                                         ? 0U
                                         : private_total / memory_observations;
        std::fprintf(stderr,
                     "mode=headless-recording\nelevated=%d\nduration_seconds=%.3f\n"
                     "cpu_percent_total_capacity=%.6f\n"
                     "working_set_average_bytes=%llu\nworking_set_maximum_bytes=%llu\n"
                     "private_average_bytes=%llu\nprivate_maximum_bytes=%llu\n"
                     "process_write_bytes=%llu\ncollector_samples=%llu\n"
                     "collector_average_ns=%.0f\ncollector_p95_ns=%.0f\ncollector_p99_ns=%.0f\n"
                     "jitter_average_ns=%.0f\njitter_p95_ns=%.0f\njitter_p99_ns=%.0f\n"
                     "collector_deadline_margin_percent=%.6f\n"
                     "collector_deadline_misses=%llu\ncollector_dropped=%llu\n"
                     "collector_failed=%llu\nactive_processes=%zu\nresume_events=%llu\n",
                     process_is_elevated() ? 1 : 0, wall_seconds, cpu_percent,
                     static_cast<unsigned long long>(working_set_average),
                     static_cast<unsigned long long>(working_set_maximum),
                     static_cast<unsigned long long>(private_average),
                     static_cast<unsigned long long>(private_maximum),
                     static_cast<unsigned long long>(
                         io_after.WriteTransferCount - io_before.WriteTransferCount),
                     static_cast<unsigned long long>(diagnostics.collection_count),
                     nanoseconds(diagnostics.collection_timing.average),
                     nanoseconds(diagnostics.collection_timing.p95),
                     nanoseconds(diagnostics.collection_timing.p99),
                     nanoseconds(diagnostics.scheduling_jitter.average),
                     nanoseconds(diagnostics.scheduling_jitter.p95),
                     nanoseconds(diagnostics.scheduling_jitter.p99),
                     100.0 - nanoseconds(diagnostics.collection_timing.p99) /
                                 10'000'000.0,
                     static_cast<unsigned long long>(diagnostics.deadline_misses),
                     static_cast<unsigned long long>(diagnostics.dropped_samples),
                     static_cast<unsigned long long>(diagnostics.failed_samples),
                     diagnostics.active_processes,
                     static_cast<unsigned long long>(diagnostics.resume_events));
    }
}
