#pragma once

#include "telemetry/types.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace blackbox::telemetry {

[[nodiscard]] MetricValue<Ratio> normalize_process_cpu(
    const MetricValue<std::chrono::nanoseconds>& previous,
    const MetricValue<std::chrono::nanoseconds>& current,
    std::chrono::steady_clock::duration elapsed,
    const MetricValue<std::uint32_t>& logical_processor_count) noexcept;

class ProcessTelemetryNormalizer final {
public:
    void normalize(const RawTelemetrySnapshot& raw,
                   std::vector<ProcessSample>& destination);
    void reset() noexcept;
    [[nodiscard]] std::size_t tracked_processes() const noexcept;

private:
    struct IdentityHash {
        [[nodiscard]] std::size_t operator()(const ProcessIdentity& identity) const noexcept;
    };

    struct PreviousObservation {
        core::MonotonicTimePoint observed_at{};
        RawProcessCounters counters{};
        std::uint64_t generation{};
    };

    std::unordered_map<ProcessIdentity, PreviousObservation, IdentityHash> previous_{};
    std::uint64_t generation_{};
};

} // namespace blackbox::telemetry
