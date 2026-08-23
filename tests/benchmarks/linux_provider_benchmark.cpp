#include "core/clock.hpp"
#include "telemetry/collection_timing.hpp"
#include "telemetry/linux/linux_telemetry_provider.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace {

[[nodiscard]] std::uint64_t parse_count(const int argc, char **argv) noexcept {
    if (argc < 2 || argv[1] == nullptr) return 16U;
    const std::string_view input{argv[1]};
    std::uint64_t result{};
    const auto parsed = std::from_chars(
        input.data(), input.data() + input.size(), result);
    return parsed.ec == std::errc{} &&
                   parsed.ptr == input.data() + input.size() &&
                   result >= 2U && result <= 256U
               ? result
               : 0U;
}

[[nodiscard]] std::uint64_t microseconds(
    const std::chrono::nanoseconds value) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(value).count());
}

} // namespace

int main(const int argc, char **argv) {
    namespace core = blackbox::core;
    namespace telemetry = blackbox::telemetry;
    namespace linux_telemetry = blackbox::telemetry::linux;

    const auto sample_count = parse_count(argc, argv);
    if (sample_count == 0U) return 2;

    core::SystemMonotonicClock clock{};
    linux_telemetry::LinuxTelemetryProvider provider{clock};
    telemetry::CollectionTimingWindow timing{};
    telemetry::RawTelemetrySnapshot snapshot{};
    const telemetry::SamplingRequest request{
        telemetry::SamplingTierSet::all()};
    std::uint64_t complete{};
    std::uint64_t partial{};
    std::uint64_t failed{};
    std::size_t maximum_processes{};

    for (std::uint64_t index = 0U; index < sample_count; ++index) {
        const auto started = clock.now();
        const auto result = provider.sample(request, snapshot);
        const auto finished = clock.now();
        timing.record(std::chrono::duration_cast<std::chrono::nanoseconds>(
            finished - started));
        maximum_processes = (std::max)(maximum_processes,
                                       snapshot.processes.size());
        switch (result.status) {
        case telemetry::ProviderSampleStatus::complete: ++complete; break;
        case telemetry::ProviderSampleStatus::partial: ++partial; break;
        case telemetry::ProviderSampleStatus::temporarily_failed: ++failed; break;
        }
    }

    rusage usage{};
    const auto has_usage = getrusage(RUSAGE_SELF, &usage) == 0;
    const auto summary = timing.summary();
    const auto peak_rss_bytes = has_usage && usage.ru_maxrss > 0
                                    ? static_cast<std::uint64_t>(usage.ru_maxrss) *
                                          1024U
                                    : 0U;
    std::printf(
        "format=1\nsamples=%llu\ncomplete=%llu\npartial=%llu\nfailed=%llu\n"
        "processes_max=%zu\naverage_us=%llu\np95_us=%llu\np99_us=%llu\n"
        "maximum_us=%llu\npeak_rss_bytes=%llu\n",
        static_cast<unsigned long long>(sample_count),
        static_cast<unsigned long long>(complete),
        static_cast<unsigned long long>(partial),
        static_cast<unsigned long long>(failed), maximum_processes,
        static_cast<unsigned long long>(microseconds(summary.average)),
        static_cast<unsigned long long>(microseconds(summary.p95)),
        static_cast<unsigned long long>(microseconds(summary.p99)),
        static_cast<unsigned long long>(microseconds(summary.maximum)),
        static_cast<unsigned long long>(peak_rss_bytes));

    constexpr auto p95_budget = std::chrono::milliseconds{250};
    constexpr auto maximum_budget = std::chrono::seconds{1};
    return failed == 0U && maximum_processes != 0U &&
                   summary.p95 <= p95_budget &&
                   summary.maximum <= maximum_budget
               ? 0
               : 3;
}
