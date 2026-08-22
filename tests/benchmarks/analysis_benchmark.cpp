#include "analysis/statistical_incident_analyzer.hpp"
#include "storage/test_incident.hpp"

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
namespace storage = blackbox::storage;
using namespace std::chrono_literals;

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

void measure(const std::size_t process_count) {
    constexpr std::size_t frames = 150U;
    constexpr std::size_t trials = 10U;
    analysis::StatisticalIncidentAnalyzer analyzer;
    const auto normal = storage::test::scaled_incident(process_count, frames, false);
    static_cast<void>(analyzer.analyze(*normal));
    std::vector<double> durations;
    durations.reserve(trials);
    std::uint64_t maximum_temporary_bytes{};
    double maximum_normal_score{};
    bool all_analyzed = true;
    for (std::size_t trial = 0U; trial < trials; ++trial) {
        const auto memory_before = working_set_bytes();
        std::atomic<bool> monitoring{true};
        std::atomic<std::uint64_t> peak{memory_before};
        std::jthread monitor{[&](const std::stop_token stop_token) {
            while (!stop_token.stop_requested() && monitoring.load()) {
                const auto observed = working_set_bytes();
                auto previous = peak.load();
                while (observed > previous &&
                       !peak.compare_exchange_weak(previous, observed)) {}
                std::this_thread::sleep_for(250us);
            }
        }};
        const auto started = std::chrono::steady_clock::now();
        const auto result = analyzer.analyze(*normal);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        monitoring.store(false);
        monitor.request_stop();
        monitor.join();
        peak.store((std::max)(peak.load(), working_set_bytes()));
        durations.push_back(std::chrono::duration<double, std::milli>{elapsed}.count());
        all_analyzed = all_analyzed && result.has_value();
        if (result) {
            for (const auto& resource : result->resources)
                maximum_normal_score = (std::max)(maximum_normal_score, resource.score);
            for (const auto& process : result->processes)
                maximum_normal_score = (std::max)(maximum_normal_score, process.score);
        }
        maximum_temporary_bytes = (std::max)(
            maximum_temporary_bytes,
            peak.load() > memory_before ? peak.load() - memory_before : 0U);
    }
    std::sort(durations.begin(), durations.end());
    const auto percentile = [&](const std::size_t numerator) {
        const auto rank = (durations.size() * numerator + 99U) / 100U;
        return durations[(std::max<std::size_t>)(1U, rank) - 1U];
    };
    const auto average = std::accumulate(durations.begin(), durations.end(), 0.0) /
                         static_cast<double>(durations.size());
    std::cout << process_count << ',' << process_count * frames << ',' << average << ','
              << percentile(95U) << ',' << percentile(99U) << ',' << durations.back()
              << ',' << maximum_temporary_bytes << ',' << maximum_normal_score << ','
              << static_cast<int>(all_analyzed) << '\n';
}

} // namespace

int main() {
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "processes,process_samples,average_ms,p95_ms,p99_ms,maximum_ms,"
                 "peak_temporary_working_set_bytes,maximum_normal_score,all_analyzed\n";
    measure(50U);
    measure(200U);
    measure(500U);
    return 0;
}
