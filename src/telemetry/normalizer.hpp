#pragma once

#include "telemetry/types.hpp"

#include <chrono>
#include <optional>

namespace blackbox::telemetry {

// Fractions over tiny TCP populations are too noisy to support an automatic
// incident claim. Keep the counter observation, but warm the derived ratio
// until a minimally useful interval population is present.
inline constexpr std::uint64_t minimum_tcp_segments_for_retransmit_fraction = 8U;

[[nodiscard]] MetricValue<Ratio>
normalize_cpu_usage(const MetricValue<CpuTimeCounters>& previous,
                    const MetricValue<CpuTimeCounters>& current,
                    std::chrono::steady_clock::duration elapsed) noexcept;

[[nodiscard]] MetricValue<BytesPerSecond>
normalize_byte_rate(const MetricValue<ByteCount>& previous, const MetricValue<ByteCount>& current,
                    std::chrono::steady_clock::duration elapsed) noexcept;

[[nodiscard]] MetricValue<std::uint64_t>
normalize_counter_delta(const MetricValue<std::uint64_t>& previous,
                        const MetricValue<std::uint64_t>& current) noexcept;

[[nodiscard]] MetricValue<Ratio>
normalize_tcp_retransmit_fraction(const MetricValue<std::uint64_t>& previous_out,
                                  const MetricValue<std::uint64_t>& current_out,
                                  const MetricValue<std::uint64_t>& previous_retransmitted,
                                  const MetricValue<std::uint64_t>& current_retransmitted) noexcept;

[[nodiscard]] MetricValue<Ratio>
normalize_stall_fraction(const MetricValue<std::uint64_t>& previous_microseconds,
                         const MetricValue<std::uint64_t>& current_microseconds,
                         std::chrono::steady_clock::duration elapsed) noexcept;

[[nodiscard]] MetricValue<ByteCount>
normalize_memory_used(const MetricValue<ByteCount>& total,
                      const MetricValue<ByteCount>& available) noexcept;

[[nodiscard]] MetricValue<Ratio>
normalize_memory_usage(const MetricValue<ByteCount>& total,
                       const MetricValue<ByteCount>& available) noexcept;

class SystemTelemetryNormalizer final {
public:
    [[nodiscard]] SystemSample normalize(const RawTelemetrySnapshot& raw) noexcept;
    void reset() noexcept;

private:
    struct PreviousObservation {
        core::MonotonicTimePoint observed_at{};
        RawSystemCounters system{};
    };

    std::optional<PreviousObservation> previous_{};
};

} // namespace blackbox::telemetry
