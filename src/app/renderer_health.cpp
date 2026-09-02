#include "app/renderer_health.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace blackbox::app {
namespace {

[[nodiscard]] double milliseconds(const std::chrono::nanoseconds value) noexcept {
    return std::chrono::duration<double, std::milli>{value}.count();
}

template <std::size_t Size>
[[nodiscard]] std::chrono::nanoseconds percentile(
    const std::array<std::chrono::nanoseconds, Size>& values,
    const std::size_t size, const double fraction) noexcept {
    if (size == 0U) return {};
    auto sorted = values;
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(size));
    const auto index = std::min(size - 1U,
                                static_cast<std::size_t>(std::ceil(fraction * size)) - 1U);
    return sorted[index];
}

} // namespace

void RendererHealthTracker::observe(const RendererFrameObservation observation) noexcept {
    if (observation.build_duration < std::chrono::nanoseconds::zero() ||
        observation.present_duration < std::chrono::nanoseconds::zero() ||
        observation.target_interval <= std::chrono::nanoseconds::zero()) {
        return;
    }
    const auto total = observation.build_duration + observation.present_duration;
    build_[next_] = observation.build_duration;
    present_[next_] = observation.present_duration;
    frame_[next_] = total;
    next_ = (next_ + 1U) % capacity;
    size_ = std::min(size_ + 1U, capacity);
    ++frames_;
    const auto hitch_threshold =
        std::max(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::milliseconds{50}),
                 observation.target_interval * 2);
    if (total > hitch_threshold) ++hitches_;
    if (!observation.present_succeeded) ++present_failures_;
}

RendererHealthSnapshot RendererHealthTracker::snapshot() const noexcept {
    RendererHealthSnapshot result{};
    result.frames = frames_;
    result.hitches = hitches_;
    result.present_failures = present_failures_;
    result.build_p95_milliseconds = milliseconds(percentile(build_, size_, 0.95));
    result.present_p95_milliseconds = milliseconds(percentile(present_, size_, 0.95));
    result.frame_p95_milliseconds = milliseconds(percentile(frame_, size_, 0.95));
    if (size_ != 0U) {
        result.frame_maximum_milliseconds =
            milliseconds(*std::max_element(frame_.begin(),
                                            frame_.begin() + static_cast<std::ptrdiff_t>(size_)));
    }
    return result;
}

void RendererHealthTracker::reset_window() noexcept {
    build_.fill({});
    present_.fill({});
    frame_.fill({});
    size_ = 0U;
    next_ = 0U;
}

} // namespace blackbox::app
