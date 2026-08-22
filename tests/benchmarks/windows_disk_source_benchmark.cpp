#include "core/clock.hpp"
#include "telemetry/windows/windows_telemetry_provider.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <thread>

namespace {

struct NativeTotals {
    std::uint64_t read_bytes{};
    std::uint64_t write_bytes{};
    std::uint32_t devices{};
};

[[nodiscard]] NativeTotals read_native_totals() noexcept {
    NativeTotals result{};
    for (std::uint32_t number = 0U; number < 32U; ++number) {
        wchar_t path[64]{};
        std::swprintf(path, 64U, L"\\\\.\\PhysicalDrive%u", number);
        const HANDLE drive = CreateFileW(
            path, 0U, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, 0U, nullptr);
        if (drive == INVALID_HANDLE_VALUE) {
            continue;
        }
        DISK_PERFORMANCE counters{};
        DWORD returned = 0U;
        if (DeviceIoControl(drive, IOCTL_DISK_PERFORMANCE, nullptr, 0U,
                            &counters, sizeof(counters), &returned, nullptr) != 0) {
            result.read_bytes += static_cast<std::uint64_t>(counters.BytesRead.QuadPart);
            result.write_bytes += static_cast<std::uint64_t>(counters.BytesWritten.QuadPart);
            ++result.devices;
            // IOCTL_DISK_PERFORMANCE increments the driver's enable reference;
            // balance every probe so the benchmark does not alter system state.
            static_cast<void>(DeviceIoControl(
                drive, IOCTL_DISK_PERFORMANCE_OFF, nullptr, 0U, nullptr, 0U,
                &returned, nullptr));
        }
        CloseHandle(drive);
    }
    return result;
}

} // namespace

int main() {
    namespace core = blackbox::core;
    namespace telemetry = blackbox::telemetry;
    namespace windows = blackbox::telemetry::windows;

    core::SystemMonotonicClock clock;
    windows::WindowsTelemetryProvider provider{clock};
    telemetry::RawTelemetrySnapshot raw;
    static_cast<void>(provider.sample({}, raw));
    const auto native_before = read_native_totals();

    std::this_thread::sleep_for(std::chrono::seconds{1});

    static_cast<void>(provider.sample({}, raw));
    const auto native_after = read_native_totals();
    const auto pdh_read = raw.system.disk_read_bytes.has_value()
                              ? raw.system.disk_read_bytes.value.value
                              : 0U;
    const auto pdh_write = raw.system.disk_write_bytes.has_value()
                               ? raw.system.disk_write_bytes.value.value
                               : 0U;
    const auto native_read = native_after.read_bytes >= native_before.read_bytes
                                 ? native_after.read_bytes - native_before.read_bytes
                                 : 0U;
    const auto native_write = native_after.write_bytes >= native_before.write_bytes
                                  ? native_after.write_bytes - native_before.write_bytes
                                  : 0U;

    constexpr std::uint32_t iterations = 100U;
    const auto started = clock.now();
    std::uint64_t checksum = 0U;
    for (std::uint32_t index = 0U; index < iterations; ++index) {
        const auto totals = read_native_totals();
        checksum ^= totals.read_bytes ^ totals.write_bytes;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        clock.now() - started);

    const auto disk_read_latency = raw.system.disk_quality.read_latency.has_value()
                                       ? raw.system.disk_quality.read_latency.value.value
                                       : -1.0;
    const auto disk_write_latency = raw.system.disk_quality.write_latency.has_value()
                                        ? raw.system.disk_quality.write_latency.value.value
                                        : -1.0;
    const auto disk_service_time = raw.system.disk_quality.service_time.has_value()
                                       ? raw.system.disk_quality.service_time.value.value
                                       : -1.0;
    const auto disk_queue_depth = raw.system.disk_quality.queue_depth.has_value()
                                      ? raw.system.disk_quality.queue_depth.value
                                      : -1.0;
    std::fprintf(stderr, "native_devices=%u\npdh_delta_read=%llu\npdh_delta_write=%llu\n"
                "native_delta_read=%llu\nnative_delta_write=%llu\n"
                "disk_read_latency_seconds=%.9f\ndisk_write_latency_seconds=%.9f\n"
                "disk_service_time_seconds=%.9f\ndisk_queue_depth=%.6f\n"
                "native_probe_average_ns=%.0f\nchecksum=%llu\n",
                native_after.devices,
                static_cast<unsigned long long>(pdh_read),
                static_cast<unsigned long long>(pdh_write),
                static_cast<unsigned long long>(native_read),
                static_cast<unsigned long long>(native_write),
                disk_read_latency, disk_write_latency, disk_service_time,
                disk_queue_depth,
                static_cast<double>(elapsed.count()) / iterations,
                static_cast<unsigned long long>(checksum));
}
