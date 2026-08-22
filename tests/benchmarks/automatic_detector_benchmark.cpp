#include "telemetry/automatic_incident_detector.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>

int main() {
    namespace core = blackbox::core;
    namespace telemetry = blackbox::telemetry;
    constexpr std::uint64_t observations = 2'000'000U;
    telemetry::AutomaticIncidentDetector detector{};
    telemetry::SystemSample sample{};
    sample.cpu_usage = telemetry::MetricValue<telemetry::Ratio>::available({0.35});
    sample.memory_usage = telemetry::MetricValue<telemetry::Ratio>::available({0.58});
    sample.disk_read_rate = telemetry::MetricValue<telemetry::BytesPerSecond>::available(
        {12.0 * 1024.0 * 1024.0});
    sample.disk_write_rate = telemetry::MetricValue<telemetry::BytesPerSecond>::available(
        {4.0 * 1024.0 * 1024.0});
    sample.network_receive_rate = telemetry::MetricValue<telemetry::BytesPerSecond>::available(
        {3.0 * 1024.0 * 1024.0});
    sample.network_transmit_rate = telemetry::MetricValue<telemetry::BytesPerSecond>::available(
        {1.0 * 1024.0 * 1024.0});

    auto* interface = static_cast<telemetry::IAutomaticIncidentDetector*>(&detector);
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t triggers = 0U;
    for (std::uint64_t index = 0; index < observations; ++index) {
        sample.observed_at = core::MonotonicTimePoint{
            std::chrono::nanoseconds{static_cast<std::int64_t>(index)}};
        triggers += interface->observe(sample).has_value();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto nanoseconds = std::chrono::duration<double, std::nano>{elapsed}.count();
    std::cout << "automatic detector observations=" << observations
              << " triggers=" << triggers
              << " ns_per_collector_sample=" << nanoseconds / observations << '\n';
    return triggers == 0U ? 0 : 1;
}
