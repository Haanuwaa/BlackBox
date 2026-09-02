#pragma once

#include "telemetry/types.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>

namespace blackbox::telemetry {

// One physical-device cumulative observation. Native providers translate
// their exact units to nanoseconds before entering this lifecycle tracker.
// weighted_time is the cumulative integral of requests in progress and is
// absent on platforms that expose no exact queue integral.
struct DiskQualityCounters {
    std::uint64_t identity{};
    std::uint64_t read_operations{};
    std::uint64_t write_operations{};
    std::uint64_t read_time_nanoseconds{};
    std::uint64_t write_time_nanoseconds{};
    std::optional<std::uint64_t> weighted_time_nanoseconds{};
    friend constexpr bool operator==(const DiskQualityCounters&,
                                     const DiskQualityCounters&) = default;
};

template <std::size_t Capacity>
class DiskQualityTracker {
    static_assert(Capacity > 0U);

public:
    [[nodiscard]] RawDiskQuality update(
        const core::MonotonicTimePoint observed_at,
        const std::span<const DiskQualityCounters> observations) noexcept {
        RawDiskQuality result{};
        if (observations.size() > Capacity || observations.empty()) {
            return temporary_result(queue_supported(observations));
        }

        for (auto& entry : entries_) entry.seen = false;
        bool have_candidate{};
        double candidate_latency{};
        double candidate_queue{};
        double candidate_concurrency{};

        for (std::size_t index = 0U; index < observations.size(); ++index) {
            const auto& current = observations[index];
            if (current.identity == 0U || duplicate(observations, index)) {
                invalidate_membership();
                return temporary_result(queue_supported(observations));
            }

            auto* entry = find(current.identity);
            if (entry == nullptr) entry = allocate(current.identity);
            if (entry == nullptr) {
                invalidate_membership();
                return temporary_result(queue_supported(observations));
            }
            entry->seen = true;

            if (!entry->active || observed_at <= entry->observed_at ||
                decreased(current, entry->counters) ||
                current.weighted_time_nanoseconds.has_value() !=
                    entry->counters.weighted_time_nanoseconds.has_value()) {
                warm(*entry, observed_at, current);
                continue;
            }

            RawDiskQuality candidate{};
            const auto read_operations =
                current.read_operations - entry->counters.read_operations;
            const auto write_operations =
                current.write_operations - entry->counters.write_operations;
            const auto read_time = current.read_time_nanoseconds -
                                   entry->counters.read_time_nanoseconds;
            const auto write_time = current.write_time_nanoseconds -
                                    entry->counters.write_time_nanoseconds;
            const auto total_operations = read_operations + write_operations;
            if (total_operations < read_operations) {
                warm(*entry, observed_at, current);
                continue;
            }

            candidate.read_latency = mean_seconds(read_time, read_operations);
            candidate.write_latency = mean_seconds(write_time, write_operations);
            candidate.service_time =
                mean_seconds_sum(read_time, write_time, total_operations);
            const auto elapsed = observed_at - entry->observed_at;
            candidate.service_concurrency =
                average_service_concurrency(read_time, write_time, elapsed);
            if (current.weighted_time_nanoseconds) {
                const auto weighted = *current.weighted_time_nanoseconds -
                                      *entry->counters.weighted_time_nanoseconds;
                const auto elapsed_nanoseconds =
                    std::chrono::duration<double, std::nano>{elapsed}.count();
                if (elapsed_nanoseconds > 0.0 && std::isfinite(elapsed_nanoseconds)) {
                    candidate.queue_depth = MetricValue<double>::available(
                        static_cast<double>(weighted) / elapsed_nanoseconds);
                } else {
                    candidate.queue_depth = temporary<double>();
                }
            }
            candidate.worst_device_id =
                MetricValue<std::uint64_t>::available(current.identity);

            const auto latency = maximum_latency(candidate);
            const auto queue = candidate.queue_depth.has_value()
                                   ? candidate.queue_depth.value
                                   : 0.0;
            const auto concurrency = candidate.service_concurrency.has_value()
                                         ? candidate.service_concurrency.value
                                         : 0.0;
            if (!have_candidate || latency > candidate_latency ||
                (latency == candidate_latency &&
                 (queue > candidate_queue ||
                  (queue == candidate_queue && concurrency > candidate_concurrency)))) {
                result = candidate;
                have_candidate = true;
                candidate_latency = latency;
                candidate_queue = queue;
                candidate_concurrency = concurrency;
            }
            warm(*entry, observed_at, current);
        }

        for (auto& entry : entries_) {
            if (entry.active && !entry.seen) entry.active = false;
        }
        return have_candidate ? result : temporary_result(queue_supported(observations));
    }

private:
    struct Entry {
        std::uint64_t identity{};
        core::MonotonicTimePoint observed_at{};
        DiskQualityCounters counters{};
        bool active{};
        bool seen{};
    };

    template <typename T>
    [[nodiscard]] static constexpr MetricValue<T> temporary() noexcept {
        return MetricValue<T>::unavailable(MetricStatus::temporarily_unavailable);
    }

    [[nodiscard]] static RawDiskQuality temporary_result(
        const bool has_queue_counter) noexcept {
        RawDiskQuality result{};
        result.read_latency = temporary<Seconds>();
        result.write_latency = temporary<Seconds>();
        result.service_time = temporary<Seconds>();
        result.queue_depth = has_queue_counter
                                 ? temporary<double>()
                                 : MetricValue<double>::unavailable(MetricStatus::unsupported);
        result.service_concurrency = temporary<double>();
        result.worst_device_id = temporary<std::uint64_t>();
        return result;
    }

    [[nodiscard]] static bool queue_supported(
        const std::span<const DiskQualityCounters> observations) noexcept {
        return !observations.empty() &&
               std::ranges::all_of(observations, [](const auto& value) {
                   return value.weighted_time_nanoseconds.has_value();
               });
    }

    [[nodiscard]] static bool duplicate(
        const std::span<const DiskQualityCounters> observations,
        const std::size_t current) noexcept {
        for (std::size_t prior = 0U; prior < current; ++prior) {
            if (observations[prior].identity == observations[current].identity) return true;
        }
        return false;
    }

    [[nodiscard]] static bool decreased(const DiskQualityCounters& current,
                                        const DiskQualityCounters& prior) noexcept {
        return current.read_operations < prior.read_operations ||
               current.write_operations < prior.write_operations ||
               current.read_time_nanoseconds < prior.read_time_nanoseconds ||
               current.write_time_nanoseconds < prior.write_time_nanoseconds ||
               (current.weighted_time_nanoseconds && prior.weighted_time_nanoseconds &&
                *current.weighted_time_nanoseconds < *prior.weighted_time_nanoseconds);
    }

    [[nodiscard]] static MetricValue<Seconds> mean_seconds(
        const std::uint64_t nanoseconds,
        const std::uint64_t operations) noexcept {
        if (operations == 0U) return temporary<Seconds>();
        const auto value = static_cast<double>(nanoseconds) /
                           static_cast<double>(operations) / 1'000'000'000.0;
        return std::isfinite(value) && value >= 0.0
                   ? MetricValue<Seconds>::available(Seconds{value})
                   : temporary<Seconds>();
    }

    [[nodiscard]] static MetricValue<Seconds> mean_seconds_sum(
        const std::uint64_t read_nanoseconds,
        const std::uint64_t write_nanoseconds,
        const std::uint64_t operations) noexcept {
        if (operations == 0U ||
            read_nanoseconds > std::numeric_limits<std::uint64_t>::max() -
                                   write_nanoseconds) {
            return temporary<Seconds>();
        }
        return mean_seconds(read_nanoseconds + write_nanoseconds, operations);
    }

    [[nodiscard]] static MetricValue<double> average_service_concurrency(
        const std::uint64_t read_nanoseconds,
        const std::uint64_t write_nanoseconds,
        const std::chrono::steady_clock::duration elapsed) noexcept {
        if (read_nanoseconds > std::numeric_limits<std::uint64_t>::max() -
                                   write_nanoseconds) {
            return temporary<double>();
        }
        const auto elapsed_nanoseconds =
            std::chrono::duration<double, std::nano>{elapsed}.count();
        if (!(elapsed_nanoseconds > 0.0) || !std::isfinite(elapsed_nanoseconds)) {
            return temporary<double>();
        }
        const auto value = static_cast<double>(read_nanoseconds + write_nanoseconds) /
                           elapsed_nanoseconds;
        return std::isfinite(value) && value >= 0.0
                   ? MetricValue<double>::available(value)
                   : temporary<double>();
    }

    [[nodiscard]] static double maximum_latency(const RawDiskQuality& value) noexcept {
        double result{};
        for (const auto* metric : {&value.read_latency, &value.write_latency,
                                   &value.service_time}) {
            if (metric->has_value()) result = std::max(result, metric->value.value);
        }
        return result;
    }

    [[nodiscard]] Entry* find(const std::uint64_t identity) noexcept {
        for (auto& entry : entries_) {
            if (entry.identity == identity) return &entry;
        }
        return nullptr;
    }

    [[nodiscard]] Entry* allocate(const std::uint64_t identity) noexcept {
        for (auto& entry : entries_) {
            if (entry.identity == 0U || !entry.active) {
                entry = Entry{};
                entry.identity = identity;
                return &entry;
            }
        }
        return nullptr;
    }

    static void warm(Entry& entry,
                     const core::MonotonicTimePoint observed_at,
                     const DiskQualityCounters& counters) noexcept {
        entry.identity = counters.identity;
        entry.observed_at = observed_at;
        entry.counters = counters;
        entry.active = true;
        entry.seen = true;
    }

    void invalidate_membership() noexcept {
        for (auto& entry : entries_) entry.active = false;
    }

    std::array<Entry, Capacity> entries_{};
};

} // namespace blackbox::telemetry
