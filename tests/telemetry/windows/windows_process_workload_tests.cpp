#include "core/clock.hpp"
#include "telemetry/process_normalizer.hpp"
#include "telemetry/windows/windows_process_collector.hpp"
#include "telemetry/windows/windows_telemetry_provider.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

namespace core = blackbox::core;
namespace telemetry = blackbox::telemetry;
namespace windows = blackbox::telemetry::windows;
using namespace std::chrono_literals;

namespace {

struct ProcessGuard {
    PROCESS_INFORMATION value{};
    ~ProcessGuard() {
        if (value.hThread != nullptr) {
            CloseHandle(value.hThread);
        }
        if (value.hProcess != nullptr) {
            CloseHandle(value.hProcess);
        }
    }
};

[[nodiscard]] std::filesystem::path fixture_path() {
    std::wstring module(32'768U, L'\0');
    const auto size = GetModuleFileNameW(
        nullptr, module.data(), static_cast<DWORD>(module.size()));
    module.resize(size);
    return std::filesystem::path{module}.parent_path() /
           "blackbox_process_fixture.exe";
}

[[nodiscard]] const telemetry::RawProcessCounters* find_raw(
    const telemetry::RawTelemetrySnapshot& raw,
    const std::uint32_t pid) {
    for (const auto& process : raw.processes) {
        if (process.identity.pid.value == pid) {
            return &process;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Windows process collector reuses a bounded live-identity handle hot set",
          "[telemetry][windows][process][performance]") {
    core::SystemMonotonicClock clock;
    windows::WindowsProcessCollector collector;
    telemetry::RawTelemetrySnapshot raw;
    raw.reset(clock.now(), telemetry::SamplingTierSet::all());
    REQUIRE(collector.collect(true, true, raw) == telemetry::MetricStatus::available);
    CHECK(collector.cached_handle_count() <=
          windows::maximum_cached_process_handles);
    CHECK(collector.cache_size() >= collector.cached_handle_count());
    const auto own_pid = static_cast<std::uint32_t>(GetCurrentProcessId());
    const auto first = find_raw(raw, own_pid);
    REQUIRE(first != nullptr);
    const auto identity = first->identity;

    raw.reset(clock.now(), telemetry::SamplingTier::normal);
    REQUIRE(collector.collect(true, false, raw) == telemetry::MetricStatus::available);
    const auto second = find_raw(raw, own_pid);
    REQUIRE(second != nullptr);
    CHECK(second->identity == identity);
    const auto diagnostics = collector.diagnostics();
    CHECK(diagnostics.handles_reused >= 1U);
    CHECK(collector.cached_handle_count() <=
          windows::maximum_cached_process_handles);

    DWORD baseline_handles{};
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &baseline_handles) != 0);
    for (std::size_t iteration = 0U; iteration < 20U; ++iteration) {
        raw.reset(clock.now(), telemetry::SamplingTier::normal);
        REQUIRE(collector.collect(true, false, raw) ==
                telemetry::MetricStatus::available);
    }
    DWORD final_handles{};
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &final_handles) != 0);
    CHECK(final_handles <= baseline_handles + 4U);
    CHECK(collector.cached_handle_count() <=
          windows::maximum_cached_process_handles);
}

TEST_CASE("Windows provider follows a short-lived CPU memory and I/O process",
          "[telemetry][windows][process][integration][workload]") {
    const auto executable = fixture_path();
    REQUIRE(std::filesystem::exists(executable));
    core::SystemMonotonicClock clock;
    windows::WindowsTelemetryProvider provider{clock};
    telemetry::RawTelemetrySnapshot raw;
    REQUIRE(provider.sample({}, raw).status ==
            telemetry::ProviderSampleStatus::complete);
    CHECK(raw.process_lifecycle_events.empty());

    std::wstring command = L"\"" + executable.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    ProcessGuard child{};
    REQUIRE(CreateProcessW(
                executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(),
                &startup, &child.value) != 0);
    if (child.value.hProcess == nullptr || child.value.hThread == nullptr) return;
    REQUIRE(CloseHandle(child.value.hThread) != 0);
    child.value.hThread = nullptr;
    const auto child_pid = static_cast<std::uint32_t>(child.value.dwProcessId);

    telemetry::ProcessTelemetryNormalizer normalizer;
    std::vector<telemetry::ProcessSample> normalized;

    const telemetry::RawProcessCounters* baseline = nullptr;
    for (std::size_t attempt = 0U; attempt < 20U && baseline == nullptr; ++attempt) {
        static_cast<void>(provider.sample({}, raw));
        baseline = find_raw(raw, child_pid);
        if (baseline == nullptr) {
            std::this_thread::sleep_for(50ms);
        }
    }
    REQUIRE(baseline != nullptr);
    const auto identity = baseline->identity;
    REQUIRE(identity.creation_token != 0U);
    REQUIRE(std::ranges::any_of(
        raw.process_lifecycle_events,
        [identity](const telemetry::RawProcessLifecycleEvent& event) {
            return event.identity == identity &&
                   event.kind == telemetry::RawProcessLifecycleKind::started;
        }));
    normalizer.normalize(raw, normalized);

    bool found_metadata = false;
    for (const auto& info : raw.process_metadata) {
        if (info.identity == identity) {
            found_metadata = true;
            REQUIRE(info.name.has_value());
            CHECK(info.name.value == "blackbox_process_fixture.exe");
            REQUIRE(info.executable_path.has_value());
            CHECK_FALSE(info.executable_path.value.empty());
        }
    }
    CHECK(found_metadata);

    bool found_sample = false;
    bool observed_cpu = false;
    bool observed_memory = false;
    bool observed_disk_write = false;
    for (std::size_t attempt = 0U;
         attempt < 25U &&
         !(observed_cpu && observed_memory && observed_disk_write);
         ++attempt) {
        std::this_thread::sleep_for(100ms);
        static_cast<void>(provider.sample(
            telemetry::SamplingRequest{
                telemetry::SamplingTier::fast | telemetry::SamplingTier::normal}, raw));
        normalizer.normalize(raw, normalized);
        for (const auto& process : normalized) {
            if (process.identity != identity) continue;
            found_sample = true;
            observed_cpu = observed_cpu ||
                           (process.cpu_usage.has_value() &&
                            process.cpu_usage.value.value > 0.0);
            observed_memory = observed_memory ||
                              (process.working_set.has_value() &&
                               process.working_set.value.value >=
                                   24U * 1024U * 1024U);
            observed_disk_write = observed_disk_write ||
                                  (process.disk_write_rate.has_value() &&
                                   process.disk_write_rate.value.value > 0.0);
            break;
        }
    }
    CHECK(found_sample);
    CHECK(observed_cpu);
    CHECK(observed_memory);
    CHECK(observed_disk_write);

    // The fixture may still be flushing its final 32 MiB write after the
    // three-second workload deadline. Loaded hosted disks can legitimately
    // take longer than five seconds without indicating a stuck process.
    REQUIRE(WaitForSingleObject(child.value.hProcess, 20'000U) == WAIT_OBJECT_0);
    DWORD exit_code = 1U;
    REQUIRE(GetExitCodeProcess(child.value.hProcess, &exit_code) != 0);
    CHECK(exit_code == 0U);
    CHECK(provider.sample({}, raw).status ==
          telemetry::ProviderSampleStatus::complete);
    CHECK(find_raw(raw, child_pid) == nullptr);
    CHECK(std::ranges::any_of(
        raw.process_lifecycle_events,
        [identity](const telemetry::RawProcessLifecycleEvent& event) {
            return event.identity == identity &&
                   event.kind == telemetry::RawProcessLifecycleKind::exited;
        }));
}
