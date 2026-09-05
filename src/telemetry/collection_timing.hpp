#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace blackbox::telemetry {

struct CollectionTimingSummary {
    std::uint64_t samples_recorded{};
    std::size_t samples_in_window{};
    std::chrono::nanoseconds average{};
    std::chrono::nanoseconds p50{};
    std::chrono::nanoseconds p95{};
    std::chrono::nanoseconds p99{};
    std::chrono::nanoseconds maximum{};
    std::chrono::nanoseconds lifetime_maximum{};
    friend constexpr bool operator==(const CollectionTimingSummary&,
                                     const CollectionTimingSummary&) = default;
};

class CollectionTimingWindow final {
public:
    static constexpr std::size_t capacity = 256U;

    void record(std::chrono::nanoseconds duration) noexcept;
    [[nodiscard]] CollectionTimingSummary summary() const noexcept;
    void reset() noexcept;

private:
    std::array<std::chrono::nanoseconds, capacity> durations_{};
    std::size_t size_{};
    std::size_t next_{};
    std::uint64_t samples_recorded_{};
    std::chrono::nanoseconds lifetime_maximum_{};
};

} // namespace blackbox::telemetry
