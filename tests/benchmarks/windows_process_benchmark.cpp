#include "core/clock.hpp"
#include "telemetry/collection_timing.hpp"
#include "telemetry/process_normalizer.hpp"
#include "telemetry/windows/windows_process_collector.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

template <typename Operation>
[[nodiscard]] double average_nanoseconds(const std::size_t iterations,
                                         Operation operation) {
    const auto started = Clock::now();
    for (std::size_t index = 0U; index < iterations; ++index) {
        operation();
    }
    const auto elapsed = std::chrono::duration<double, std::nano>{
        Clock::now() - started};
    return elapsed.count() / static_cast<double>(iterations);
}

[[nodiscard]] std::uint32_t toolhelp_count() noexcept {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0U;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::uint32_t count = 0U;
    for (BOOL more = Process32FirstW(snapshot, &entry); more != FALSE;
         more = Process32NextW(snapshot, &entry)) {
        ++count;
        entry.dwSize = sizeof(entry);
    }
    CloseHandle(snapshot);
    return count;
}

[[nodiscard]] std::uint32_t psapi_count() noexcept {
    thread_local std::array<DWORD, 8'192U> pids{};
    DWORD bytes = 0U;
    return EnumProcesses(pids.data(), static_cast<DWORD>(sizeof(pids)), &bytes) != 0
               ? bytes / sizeof(DWORD)
               : 0U;
}

struct PdhProcessQuery {
    PdhProcessQuery() {
        if (PdhOpenQueryW(nullptr, 0U, &query) == ERROR_SUCCESS &&
            PdhAddEnglishCounterW(query, L"\\Process(*)\\ID Process", 0U,
                                  &counter) == ERROR_SUCCESS) {
            static_cast<void>(PdhCollectQueryData(query));
        }
    }
    ~PdhProcessQuery() {
        if (query != nullptr) {
            PdhCloseQuery(query);
        }
    }
    [[nodiscard]] std::uint32_t collect() noexcept {
        if (query == nullptr || counter == nullptr ||
            PdhCollectQueryData(query) != ERROR_SUCCESS) {
            return 0U;
        }
        DWORD bytes = static_cast<DWORD>(buffer.size());
        DWORD count = 0U;
        return PdhGetRawCounterArrayW(
                   counter, &bytes, &count,
                   reinterpret_cast<PDH_RAW_COUNTER_ITEM_W*>(buffer.data())) ==
                       ERROR_SUCCESS
                   ? count
                   : 0U;
    }
    PDH_HQUERY query{};
    PDH_HCOUNTER counter{};
    alignas(PDH_RAW_COUNTER_ITEM_W) std::array<std::byte, 512U * 1024U> buffer{};
};

[[nodiscard]] bool query_current_process_once() noexcept {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, GetCurrentProcessId());
    if (process == nullptr) {
        return false;
    }
    FILETIME creation{}, exit{}, kernel{}, user{};
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    IO_COUNTERS io{};
    const bool result = GetProcessTimes(process, &creation, &exit, &kernel, &user) != 0 &&
                        GetProcessMemoryInfo(process, &memory, sizeof(memory)) != 0 &&
                        GetProcessIoCounters(process, &io) != 0;
    CloseHandle(process);
    return result;
}

void fill_processes(blackbox::telemetry::RawTelemetrySnapshot& raw,
                    const std::size_t count,
                    const std::uint64_t step) {
    namespace telemetry = blackbox::telemetry;
    raw.reset(Clock::time_point{std::chrono::seconds{static_cast<long long>(step)}},
              telemetry::SamplingTier::normal);
    raw.system.logical_processor_count =
        telemetry::MetricValue<std::uint32_t>::available(12U);
    raw.processes.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        telemetry::RawProcessCounters process{};
        process.identity = telemetry::ProcessIdentity{
            telemetry::ProcessId{static_cast<std::uint32_t>(index + 1U)},
            static_cast<std::uint64_t>(index + 100U)};
        process.cpu_time = telemetry::MetricValue<std::chrono::nanoseconds>::available(
            std::chrono::milliseconds{static_cast<long long>(step * 10U + index)});
        process.working_set = telemetry::MetricValue<telemetry::ByteCount>::available(
            telemetry::ByteCount{64U * 1024U * 1024U});
        process.disk_read_bytes = telemetry::MetricValue<telemetry::ByteCount>::available(
            telemetry::ByteCount{step * 4096U + index});
        process.disk_write_bytes = telemetry::MetricValue<telemetry::ByteCount>::available(
            telemetry::ByteCount{step * 2048U + index});
        raw.processes.push_back(process);
    }
}

} // namespace

int main() {
    namespace core = blackbox::core;
    namespace telemetry = blackbox::telemetry;
    namespace windows = blackbox::telemetry::windows;

    constexpr std::size_t iterations = 100U;
    std::uint64_t checksum = 0U;
    const auto toolhelp_ns = average_nanoseconds(iterations, [&] {
        checksum += toolhelp_count();
    });
    const auto psapi_ns = average_nanoseconds(iterations, [&] {
        checksum += psapi_count();
    });
    auto pdh = std::make_unique<PdhProcessQuery>();
    const auto pdh_ns = average_nanoseconds(iterations, [&] {
        checksum += pdh->collect();
    });
    const auto toolhelp_processes = toolhelp_count();
    const auto psapi_processes = psapi_count();
    const auto pdh_processes = pdh->collect();

    core::SystemMonotonicClock clock;
    windows::WindowsProcessCollector collector;
    telemetry::RawTelemetrySnapshot raw;
    telemetry::CollectionTimingWindow timing;
    for (std::size_t index = 0U; index < iterations; ++index) {
        raw.reset(clock.now(), telemetry::SamplingTierSet::all());
        const auto started = clock.now();
        static_cast<void>(collector.collect(true, index == 0U, raw));
        timing.record(std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock.now() - started));
        checksum += raw.processes.size();
    }
    const auto native = timing.summary();
    const auto collector_diagnostics = collector.diagnostics();
    std::fprintf(stderr,
                 "sizeof_process_sample=%zu\nsizeof_process_frame=%zu\n"
                 "actual_enumerated=%u\nactual_sampled=%u\nactual_inaccessible=%u\n"
                 "toolhelp_processes=%u\npsapi_processes=%u\npdh_instances=%u\n"
                 "toolhelp_enumeration_average_ns=%.0f\n"
                 "psapi_enumeration_average_ns=%.0f\n"
                 "pdh_enumeration_average_ns=%.0f\n"
                 "native_full_average_ns=%.0f\nnative_full_p95_ns=%lld\n"
                 "native_full_p99_ns=%lld\nnative_metadata_cache=%zu\n"
                 "native_cached_handles=%zu\n",
                 sizeof(telemetry::ProcessSample), sizeof(telemetry::ProcessFrame),
                 raw.process_diagnostics.enumerated,
                 raw.process_diagnostics.sampled,
                 raw.process_diagnostics.inaccessible,
                 toolhelp_processes, psapi_processes, pdh_processes,
                 toolhelp_ns, psapi_ns, pdh_ns,
                 static_cast<double>(native.average.count()),
                 static_cast<long long>(native.p95.count()),
                 static_cast<long long>(native.p99.count()),
                 collector.cache_size(), collector.cached_handle_count());
    std::fprintf(stderr,
                 "native_last_handles_opened=%llu\n"
                 "native_last_handles_reused=%llu\n"
                 "native_last_handle_open_failures=%llu\n",
                 static_cast<unsigned long long>(collector_diagnostics.handles_opened),
                 static_cast<unsigned long long>(collector_diagnostics.handles_reused),
                 static_cast<unsigned long long>(
                     collector_diagnostics.handle_open_failures));

    for (const std::size_t scale : {50U, 200U, 500U}) {
        telemetry::ProcessTelemetryNormalizer normalizer;
        std::vector<telemetry::ProcessSample> output;
        fill_processes(raw, scale, 1U);
        normalizer.normalize(raw, output);
        const auto normalization_ns = average_nanoseconds(1'000U, [&] {
            static std::uint64_t step = 2U;
            fill_processes(raw, scale, step++);
            normalizer.normalize(raw, output);
            checksum += output.size();
        });
        const auto query_ns = average_nanoseconds(25U, [&] {
            for (std::size_t index = 0U; index < scale; ++index) {
                checksum += query_current_process_once() ? 1U : 0U;
            }
        });
        std::fprintf(stderr,
                     "scale_%zu_normalization_ns=%.0f\n"
                     "scale_%zu_query_cycle_ns=%.0f\n",
                     scale, normalization_ns, scale, query_ns);
    }
    std::fprintf(stderr, "checksum=%llu\n",
                 static_cast<unsigned long long>(checksum));
}
