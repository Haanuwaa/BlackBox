#include "telemetry/collection_timing.hpp"

#include <algorithm>

namespace blackbox::telemetry {

void CollectionTimingWindow::record(std::chrono::nanoseconds duration) noexcept {
    if (duration < std::chrono::nanoseconds::zero()) {
        duration = std::chrono::nanoseconds::zero();
    }
    durations_[next_] = duration;
    next_ = (next_ + 1U) % capacity;
    size_ = std::min(size_ + 1U, capacity);
    ++samples_recorded_;
    lifetime_maximum_ = std::max(lifetime_maximum_, duration);
}

CollectionTimingSummary CollectionTimingWindow::summary() const noexcept {
    CollectionTimingSummary result{};
    result.samples_recorded = samples_recorded_;
    result.samples_in_window = size_;
    result.lifetime_maximum = lifetime_maximum_;
    if (size_ == 0U) {
        return result;
    }

    std::array<std::chrono::nanoseconds, capacity> sorted{};
    std::copy_n(durations_.begin(), size_, sorted.begin());
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(size_));

    std::chrono::nanoseconds total{};
    for (std::size_t index = 0U; index < size_; ++index) {
        total += sorted[index];
    }

    const auto percentile_index = [this](const std::size_t percentile) noexcept {
        // Nearest-rank percentile, converted to a zero-based index.
        const auto rank = (percentile * size_ + 99U) / 100U;
        return std::min(rank == 0U ? 0U : rank - 1U, size_ - 1U);
    };

    result.average = total / static_cast<std::int64_t>(size_);
    result.p50 = sorted[percentile_index(50U)];
    result.p95 = sorted[percentile_index(95U)];
    result.p99 = sorted[percentile_index(99U)];
    result.maximum = sorted[size_ - 1U];
    return result;
}

void CollectionTimingWindow::reset() noexcept {
    durations_.fill(std::chrono::nanoseconds{});
    size_ = 0U;
    next_ = 0U;
    samples_recorded_ = 0U;
    lifetime_maximum_ = {};
}

} // namespace blackbox::telemetry
