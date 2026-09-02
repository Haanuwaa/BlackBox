#pragma once

#include "core/clock.hpp"

#include <algorithm>
#include <chrono>

namespace blackbox::app {

// The live model changes at most four times per second. A 30 Hz presentation
// ceiling keeps pointer and keyboard feedback responsive without rebuilding
// and presenting identical ImGui command streams at the monitor refresh rate.
inline constexpr auto visible_frame_interval = std::chrono::milliseconds{33};

class VisibleFrameScheduler final {
public:
    [[nodiscard]] bool frame_due(const core::MonotonicTimePoint now) noexcept {
        if (!initialized_) {
            next_frame_at_ = now;
            initialized_ = true;
        }
        if (now < next_frame_at_) {
            return false;
        }
        do {
            next_frame_at_ += visible_frame_interval;
        } while (next_frame_at_ <= now);
        return true;
    }

    [[nodiscard]] std::chrono::milliseconds
    wait_timeout(const core::MonotonicTimePoint now) const noexcept {
        if (!initialized_ || now >= next_frame_at_) {
            return std::chrono::milliseconds::zero();
        }
        const auto remaining = next_frame_at_ - now;
        auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        if (timeout < remaining) {
            timeout += std::chrono::milliseconds{1};
        }
        return std::clamp(timeout, std::chrono::milliseconds{1}, visible_frame_interval);
    }

    void reset() noexcept { initialized_ = false; }

private:
    core::MonotonicTimePoint next_frame_at_{};
    bool initialized_{};
};

} // namespace blackbox::app
