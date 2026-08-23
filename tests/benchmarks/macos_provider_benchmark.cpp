#include "core/clock.hpp"
#include "telemetry/collection_timing.hpp"
#include "telemetry/macos/macos_telemetry_provider.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>

int main(int argc, char** argv) {
    std::size_t samples = 32U;
    if (argc == 2) {
        const auto parsed = std::strtoull(argv[1], nullptr, 10);
        if (parsed == 0U || parsed > 256U) return 2;
        samples = static_cast<std::size_t>(parsed);
    } else if (argc != 1) {
        return 2;
    }

    blackbox::core::SystemMonotonicClock clock;
    blackbox::telemetry::macos::MacosTelemetryProvider provider{clock};
    blackbox::telemetry::RawTelemetrySnapshot snapshot;
    blackbox::telemetry::CollectionTimingWindow timing;
    std::size_t failed{};
    std::size_t maximum_processes{};
    for (std::size_t index = 0U; index < samples; ++index) {
        const auto started = clock.now();
        const auto result = provider.sample({}, snapshot);
        timing.record(std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock.now() - started));
        if (result.status == blackbox::telemetry::ProviderSampleStatus::temporarily_failed) {
            ++failed;
        }
        maximum_processes = std::max(maximum_processes, snapshot.processes.size());
    }
    const auto summary = timing.summary();
    std::printf("format=1\nsamples=%zu\nfailed=%zu\nprocesses_max=%zu\n"
                "p95_us=%lld\nmaximum_us=%lld\ncompleted=1\n",
                samples, failed, maximum_processes,
                static_cast<long long>(summary.p95.count() / 1'000LL),
                static_cast<long long>(summary.maximum.count() / 1'000LL));
    return failed == 0U && maximum_processes != 0U ? 0 : 1;
}
