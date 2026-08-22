#include "analysis/incident_clustering.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace analysis = blackbox::analysis;

namespace {

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

[[nodiscard]] std::vector<analysis::IncidentClusterInput> inputs(
    const std::size_t count) {
    std::vector<analysis::IncidentClusterInput> result;
    result.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        analysis::IncidentFeatureVector feature{};
        feature.incident_id = static_cast<std::int64_t>(index + 1U);
        feature.created_utc_milliseconds = static_cast<std::int64_t>(index + 1U);
        for (std::size_t dimension = 0U;
             dimension < analysis::incident_feature_dimension_count; ++dimension) {
            feature.available[dimension] = true;
            const auto family = static_cast<double>(index % 8U) / 10.0;
            feature.values[dimension] = std::clamp(
                family + static_cast<double>(dimension % 2U) * 0.01, 0.0, 1.0);
        }
        result.push_back({feature, {}});
    }
    return result;
}

void measure(const std::size_t count) {
    constexpr std::size_t trials = 20U;
    const auto fixture = inputs(count);
    const auto reference = analysis::cluster_incidents(fixture);
    std::vector<double> durations;
    durations.reserve(trials);
    bool stable = true;
    for (std::size_t trial = 0U; trial < trials; ++trial) {
        const auto started = std::chrono::steady_clock::now();
        const auto result = analysis::cluster_incidents(fixture);
        durations.push_back(std::chrono::duration<double, std::milli>{
            std::chrono::steady_clock::now() - started}.count());
        stable = stable && result == reference;
    }
    const auto memory_before = working_set_bytes();
    std::atomic<bool> monitoring{true};
    std::atomic<std::uint64_t> peak{memory_before};
    std::jthread monitor{[&](const std::stop_token stop_token) {
        while (!stop_token.stop_requested() && monitoring.load()) {
            const auto observed = working_set_bytes();
            auto previous = peak.load();
            while (observed > previous &&
                   !peak.compare_exchange_weak(previous, observed)) {}
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
    }};
    for (std::size_t trial = 0U; trial < trials; ++trial)
        static_cast<void>(analysis::cluster_incidents(fixture));
    monitoring.store(false);
    monitor.request_stop();
    monitor.join();
    peak.store((std::max)(peak.load(), working_set_bytes()));
    const auto peak_temporary_bytes = peak.load() > memory_before
        ? peak.load() - memory_before : 0U;
    std::sort(durations.begin(), durations.end());
    const auto average = std::accumulate(durations.begin(), durations.end(), 0.0) /
                         static_cast<double>(durations.size());
    const auto percentile = [&](const std::size_t numerator) {
        const auto rank = (durations.size() * numerator + 99U) / 100U;
        return durations[(std::max<std::size_t>)(1U, rank) - 1U];
    };
    const auto feature_bytes = count * sizeof(analysis::IncidentClusterInput);
    std::cout << count << ',' << average << ',' << percentile(95U) << ','
              << durations.back() << ',' << feature_bytes << ','
              << peak_temporary_bytes << ','
              << reference.clusters.size() << ','
              << reference.noise_incident_ids.size() << ',' << (stable ? 1 : 0) << '\n';
}

} // namespace

int main() {
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "incidents,average_ms,p95_ms,maximum_ms,input_object_bytes,"
                 "peak_temporary_working_set_bytes,clusters,noise,stable\n";
    measure(32U);
    measure(128U);
    measure(512U);
}
