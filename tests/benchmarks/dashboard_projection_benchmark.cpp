#include "app/dashboard_projection.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <vector>

namespace app = blackbox::app;
namespace telemetry = blackbox::telemetry;

int main() {
    constexpr std::array sizes{50U, 200U, 500U, 8'192U};
    for (const auto size : sizes) {
        std::vector<telemetry::ProcessSample> fixture(size);
        for (std::size_t index = 0U; index < fixture.size(); ++index) {
            fixture[index].identity = {telemetry::ProcessId{static_cast<std::uint32_t>(index + 1U)},
                                       index + 1U};
            fixture[index].cpu_usage = telemetry::MetricValue<telemetry::Ratio>::available(
                telemetry::Ratio{static_cast<double>((index * 37U) % 100U) / 100.0});
            fixture[index].working_set = telemetry::MetricValue<telemetry::ByteCount>::available(
                telemetry::ByteCount{index * 4'096U});
        }
        std::chrono::nanoseconds elapsed{};
        constexpr std::size_t repetitions = 32U;
        for (std::size_t repetition = 0U; repetition < repetitions; ++repetition) {
            auto values = fixture;
            const auto started = std::chrono::steady_clock::now();
            app::select_top_dashboard_processes(values, 50U);
            elapsed += std::chrono::steady_clock::now() - started;
        }
        const auto average = std::chrono::duration<double, std::milli>{elapsed}.count() /
                             static_cast<double>(repetitions);
        std::cout << "processes=" << size << " average_ms=" << average << '\n';
        if (average > 100.0) return 1;
    }
    return 0;
}
