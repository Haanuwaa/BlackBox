#pragma once

#include "core/clock.hpp"

#include <algorithm>
#include <chrono>

namespace blackbox::app {

// The live model changes at most four times per second. Idle windows therefore
// do not need to rebuild identical ImGui command streams at monitor refresh
// rate. Recent direct interaction gets a short 60 Hz window so scrolling,
// dragging, hover inspection, and keyboard focus do not inherit the idle cap.
inline constexpr auto visible_idle_frame_interval = std::chrono::milliseconds{33};
inline constexpr auto visible_interactive_frame_interval = std::chrono::milliseconds{16};
inline constexpr auto visible_interaction_hold = std::chrono::milliseconds{300};

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
        // Anchor the next deadline to the frame that is actually presented.
        // A late wake-up never creates a catch-up frame or a shortened first
        // idle interval when the interaction window expires.
        next_frame_at_ = now + frame_interval(now);
        return true;
    }

    void note_interaction(const core::MonotonicTimePoint now) noexcept {
        interactive_until_ = std::max(interactive_until_, now + visible_interaction_hold);
        if (initialized_) {
            next_frame_at_ =
                std::min(next_frame_at_, now + visible_interactive_frame_interval);
        }
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
        return std::clamp(timeout, std::chrono::milliseconds{1}, frame_interval(now));
    }

    void reset() noexcept {
        initialized_ = false;
        interactive_until_ = {};
    }

private:
    [[nodiscard]] std::chrono::milliseconds
    frame_interval(const core::MonotonicTimePoint now) const noexcept {
        return now < interactive_until_ ? visible_interactive_frame_interval
                                        : visible_idle_frame_interval;
    }

    core::MonotonicTimePoint next_frame_at_{};
    core::MonotonicTimePoint interactive_until_{};
    bool initialized_{};
};

} // namespace blackbox::app
