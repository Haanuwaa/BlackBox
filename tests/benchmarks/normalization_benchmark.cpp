#include "telemetry/normalizer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>

int main() {
    namespace telemetry = blackbox::telemetry;
    constexpr std::size_t iterations = 10'000'000U;
    constexpr auto elapsed = std::chrono::seconds{1};
    double checksum = 0.0;

    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 1U; index <= iterations; ++index) {
        const auto previous = telemetry::MetricValue<telemetry::ByteCount>::available(
            telemetry::ByteCount{static_cast<std::uint64_t>(index - 1U) * 4096U});
        const auto current = telemetry::MetricValue<telemetry::ByteCount>::available(
            telemetry::ByteCount{static_cast<std::uint64_t>(index) * 4096U});
        checksum += telemetry::normalize_byte_rate(previous, current, elapsed).value.value;
    }
    const auto duration = std::chrono::steady_clock::now() - started;
    const std::chrono::duration<double, std::nano> nanoseconds{duration};

    std::cout << "iterations=" << iterations << '\n'
              << "total_ns=" << nanoseconds.count() << '\n'
              << "ns_per_normalization=" << nanoseconds.count() / static_cast<double>(iterations) << '\n'
              << "checksum=" << checksum << '\n';
}
