#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>

namespace blackbox::telemetry {

enum class NetworkInterfaceTrackerError : std::uint8_t {
    invalid_identity,
    duplicate_identity,
    capacity_exceeded,
    counter_overflow,
};

struct NetworkInterfaceState {
    std::uint64_t active_interfaces{};
    std::uint64_t change_counter{};
    friend constexpr bool operator==(const NetworkInterfaceState&,
                                     const NetworkInterfaceState&) = default;
};

// Tracks membership only. Providers retain ownership of native enumeration and
// byte counters; this fixed-capacity helper prevents interface arrival/removal
// from requiring dynamic storage or platform branches in normalization.
template <std::size_t Capacity>
class NetworkInterfaceTracker {
public:
    [[nodiscard]] std::expected<NetworkInterfaceState,
                                NetworkInterfaceTrackerError>
    update(const std::span<const std::uint64_t> current) noexcept {
        if (current.size() > Capacity) {
            return std::unexpected{
                NetworkInterfaceTrackerError::capacity_exceeded};
        }
        for (std::size_t index = 0U; index < current.size(); ++index) {
            if (current[index] == 0U) {
                return std::unexpected{
                    NetworkInterfaceTrackerError::invalid_identity};
            }
            const auto prior = current.first(index);
            if (std::find(prior.begin(), prior.end(), current[index]) !=
                prior.end()) {
                return std::unexpected{
                    NetworkInterfaceTrackerError::duplicate_identity};
            }
        }

        std::uint64_t changes{};
        if (initialized_) {
            const auto previous =
                std::span<const std::uint64_t>{previous_.data(), count_};
            for (const auto identity : current) {
                if (std::find(previous.begin(), previous.end(), identity) ==
                    previous.end()) {
                    ++changes;
                }
            }
            for (std::size_t index = 0U; index < count_; ++index) {
                if (std::find(current.begin(), current.end(), previous_[index]) ==
                    current.end()) {
                    ++changes;
                }
            }
        }
        if (changes > std::numeric_limits<std::uint64_t>::max() -
                          change_counter_) {
            return std::unexpected{
                NetworkInterfaceTrackerError::counter_overflow};
        }

        std::copy(current.begin(), current.end(), previous_.begin());
        count_ = current.size();
        initialized_ = true;
        change_counter_ += changes;
        return NetworkInterfaceState{
            static_cast<std::uint64_t>(current.size()), change_counter_};
    }

private:
    std::array<std::uint64_t, Capacity> previous_{};
    std::size_t count_{};
    std::uint64_t change_counter_{};
    bool initialized_{};
};

} // namespace blackbox::telemetry
