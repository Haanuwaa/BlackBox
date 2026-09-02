#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace blackbox::app {

struct RendererFrameObservation {
    std::chrono::nanoseconds build_duration{};
    std::chrono::nanoseconds present_duration{};
    std::chrono::nanoseconds target_interval{};
    bool present_succeeded{true};
};

struct RendererHealthSnapshot {
    std::uint64_t frames{};
    std::uint64_t hitches{};
    std::uint64_t present_failures{};
    double build_p95_milliseconds{};
    double present_p95_milliseconds{};
    double frame_p95_milliseconds{};
    double frame_maximum_milliseconds{};
};

// Main-thread, allocation-free renderer instrumentation. It observes only
// BlackBox frame construction/submission and makes no whole-system GPU claim.
class RendererHealthTracker final {
public:
    void observe(RendererFrameObservation observation) noexcept;
    [[nodiscard]] RendererHealthSnapshot snapshot() const noexcept;
    void reset_window() noexcept;

private:
    static constexpr std::size_t capacity = 256U;
    std::array<std::chrono::nanoseconds, capacity> build_{};
    std::array<std::chrono::nanoseconds, capacity> present_{};
    std::array<std::chrono::nanoseconds, capacity> frame_{};
    std::size_t size_{};
    std::size_t next_{};
    std::uint64_t frames_{};
    std::uint64_t hitches_{};
    std::uint64_t present_failures_{};
};

} // namespace blackbox::app
