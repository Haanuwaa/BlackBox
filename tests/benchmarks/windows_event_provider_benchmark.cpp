#include "telemetry/windows/windows_system_event_provider.hpp"
#include "telemetry/event_collector.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

[[nodiscard]] std::uint64_t parse_or(const char* text,
                                     const std::uint64_t fallback) noexcept {
    if (text == nullptr) return fallback;
    const std::string_view input{text};
    std::uint64_t value{};
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size()
               ? value : fallback;
}

[[nodiscard]] std::uint64_t file_time_value(const FILETIME value) noexcept {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
           static_cast<std::uint64_t>(value.dwLowDateTime);
}

} // namespace

int main(int argc, char** argv) {
    namespace core = blackbox::core;
    namespace telemetry = blackbox::telemetry;
    namespace windows = blackbox::telemetry::windows;

    const auto duration_ms = argc > 1 ? parse_or(argv[1], 1'000U) : 1'000U;
    struct Case {
        const char* name;
        telemetry::EventProviderConfiguration configuration;
    };
    const std::array cases{
        Case{"disabled", {false, false, false, false, false, false, false, false, false, false}},
        Case{"power", {true, false, false, false, false, false, false, false, false, false}},
        Case{"device", {false, true, false, false, false, false, false, false, false, false}},
        Case{"audio", {false, false, true, false, false, false, false, false, false, false}},
        Case{"service", {false, false, false, true, false, false, false, false, false, false}},
        Case{"defender", {false, false, false, false, true, false, false, false, false, false}},
        Case{"windows_update", {false, false, false, false, false, true, false, false, false, false}},
        Case{"application_hang", {false, false, false, false, false, false, true, false, false, false}},
        Case{"dns_client", {false, false, false, false, false, false, false, true, false, false}},
        Case{"display_driver", {false, false, false, false, false, false, false, false, true, false}},
        Case{"storage", {false, false, false, false, false, false, false, false, false, true}},
    };

    for (const auto& current : cases) {
        windows::WindowsSystemEventProvider provider;
        FILETIME created{}, exited{}, kernel_before{}, user_before{};
        FILETIME kernel_after{}, user_after{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited,
                        &kernel_before, &user_before);
        const auto wall_started = std::chrono::steady_clock::now();
        const auto start_status = provider.start(current.configuration);
        const auto deadline = wall_started + std::chrono::milliseconds{duration_ms};
        std::array<core::SystemEvent, telemetry::maximum_events_per_poll> events{};
        std::uint64_t polls{};
        std::uint64_t recorded{};
        std::uint64_t native_dropped{};
        auto poll_status = start_status;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto result = provider.poll(core::MonotonicClock::now(), events);
            poll_status = result.status;
            recorded += result.event_count;
            native_dropped = result.native_events_dropped;
            ++polls;
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        provider.stop();
        const auto wall_finished = std::chrono::steady_clock::now();
        GetProcessTimes(GetCurrentProcess(), &created, &exited,
                        &kernel_after, &user_after);
        const auto process_100ns =
            file_time_value(kernel_after) - file_time_value(kernel_before) +
            file_time_value(user_after) - file_time_value(user_before);
        const auto wall_seconds =
            std::chrono::duration<double>(wall_finished - wall_started).count();
        const auto processors = (std::max)(1U, std::thread::hardware_concurrency());
        const auto cpu_percent =
            static_cast<double>(process_100ns) / (wall_seconds * 10'000'000.0) /
            static_cast<double>(processors) * 100.0;
        std::fprintf(stderr,
                     "source=%s duration_seconds=%.3f start_status=%d poll_status=%d "
                     "polls=%llu events=%llu native_dropped=%llu "
                     "cpu_percent_total_capacity=%.6f\n",
                     current.name, wall_seconds, static_cast<int>(start_status),
                     static_cast<int>(poll_status),
                     static_cast<unsigned long long>(polls),
                     static_cast<unsigned long long>(recorded),
                     static_cast<unsigned long long>(native_dropped), cpu_percent);
    }
}
