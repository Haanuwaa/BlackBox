#pragma once

#include "telemetry/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace blackbox::telemetry::windows {

struct IoEntityCounters {
    std::uint64_t identity{};
    std::uint64_t first_bytes{};
    std::uint64_t second_bytes{};
    friend constexpr bool operator==(const IoEntityCounters&, const IoEntityCounters&) = default;
};

struct IoAggregateCounters {
    MetricValue<ByteCount> first{};
    MetricValue<ByteCount> second{};
};

// Produces monotonic aggregate counters from a changing set of per-device
// counters. New/reappearing entities establish a baseline; removed entities
// contribute no negative delta; and a reset invalidates only the affected
// aggregate for that observation. The fixed capacity keeps the sampling path
// allocation-free.
template <std::size_t Capacity = 128U>
class IoCounterTracker {
public:
    [[nodiscard]] IoAggregateCounters update(
        const std::span<const IoEntityCounters> counters) noexcept {
        if (!valid_input(counters)) {
            return unavailable();
        }

        for (auto& slot : slots_) {
            slot.seen = false;
            if (slot.active && !contains(counters, slot.identity)) {
                slot.active = false;
            }
        }

        auto first_status = MetricStatus::available;
        auto second_status = MetricStatus::available;
        for (const auto& counter : counters) {
            auto* slot = find_active(counter.identity);
            if (slot == nullptr) {
                slot = find_free();
                if (slot == nullptr) {
                    return unavailable();
                }
                *slot = Slot{counter.identity, counter.first_bytes,
                             counter.second_bytes, true, true};
                continue;
            }

            slot->seen = true;
            accumulate(counter.first_bytes, slot->first_bytes,
                       first_total_, first_status);
            accumulate(counter.second_bytes, slot->second_bytes,
                       second_total_, second_status);
        }

        for (auto& slot : slots_) {
            if (slot.active && !slot.seen) {
                slot.active = false;
            }
        }

        return IoAggregateCounters{
            make_value(first_total_, first_status),
            make_value(second_total_, second_status)};
    }

private:
    struct Slot {
        std::uint64_t identity{};
        std::uint64_t first_bytes{};
        std::uint64_t second_bytes{};
        bool active{};
        bool seen{};
    };

    [[nodiscard]] static constexpr IoAggregateCounters unavailable() noexcept {
        return IoAggregateCounters{
            MetricValue<ByteCount>::unavailable(MetricStatus::temporarily_unavailable),
            MetricValue<ByteCount>::unavailable(MetricStatus::temporarily_unavailable)};
    }

    [[nodiscard]] static constexpr MetricValue<ByteCount> make_value(
        const std::uint64_t value,
        const MetricStatus status) noexcept {
        return status == MetricStatus::available
                   ? MetricValue<ByteCount>::available(ByteCount{value})
                   : MetricValue<ByteCount>::unavailable(status);
    }

    [[nodiscard]] static bool valid_input(
        const std::span<const IoEntityCounters> counters) noexcept {
        if (counters.size() > Capacity) {
            return false;
        }
        for (std::size_t left = 0U; left < counters.size(); ++left) {
            for (std::size_t right = left + 1U; right < counters.size(); ++right) {
                if (counters[left].identity == counters[right].identity) {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] static bool contains(
        const std::span<const IoEntityCounters> counters,
        const std::uint64_t identity) noexcept {
        for (const auto& counter : counters) {
            if (counter.identity == identity) {
                return true;
            }
        }
        return false;
    }

    static void accumulate(const std::uint64_t current,
                           std::uint64_t& previous,
                           std::uint64_t& total,
                           MetricStatus& status) noexcept {
        if (current < previous) {
            status = MetricStatus::temporarily_unavailable;
            previous = current;
            return;
        }
        const auto delta = current - previous;
        previous = current;
        if (delta > std::numeric_limits<std::uint64_t>::max() - total) {
            status = MetricStatus::temporarily_unavailable;
            return;
        }
        total += delta;
    }

    [[nodiscard]] Slot* find_active(const std::uint64_t identity) noexcept {
        for (auto& slot : slots_) {
            if (slot.active && slot.identity == identity) {
                return &slot;
            }
        }
        return nullptr;
    }

    [[nodiscard]] Slot* find_free() noexcept {
        for (auto& slot : slots_) {
            if (!slot.active) {
                return &slot;
            }
        }
        return nullptr;
    }

    std::array<Slot, Capacity> slots_{};
    std::uint64_t first_total_{};
    std::uint64_t second_total_{};
};

} // namespace blackbox::telemetry::windows
