#pragma once

#include "core/system_event.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace blackbox::telemetry {

struct EventProviderConfiguration {
    bool power_events{true};
    bool device_events{true};
    bool audio_device_events{true};
    bool service_events{true};
    bool defender_events{true};
    bool windows_update_events{true};
    bool application_events{true};
    bool network_events{true};
    bool graphics_events{true};
    bool storage_events{true};
    friend constexpr bool operator==(const EventProviderConfiguration&,
                                     const EventProviderConfiguration&) = default;
};

struct EventProviderCapabilities {
    bool power_events{};
    bool device_events{};
    bool audio_device_events{};
    bool service_events{};
    bool defender_events{};
    bool windows_update_events{};
    bool application_events{};
    bool network_events{};
    bool graphics_events{};
    bool storage_events{};
    friend constexpr bool operator==(const EventProviderCapabilities&,
                                     const EventProviderCapabilities&) = default;
};

enum class EventProviderStatus : std::uint8_t {
    complete,
    partial,
    temporarily_failed,
};

struct EventProviderPollResult {
    EventProviderStatus status{EventProviderStatus::complete};
    std::size_t event_count{};
    std::uint64_t native_events_dropped{};
    friend constexpr bool operator==(const EventProviderPollResult&,
                                     const EventProviderPollResult&) = default;
};

class ISystemEventProvider {
public:
    virtual ~ISystemEventProvider() = default;

    // Lifecycle runs on the event collector thread. Poll writes at most the
    // caller-provided span and performs no persistent I/O.
    [[nodiscard]] virtual EventProviderStatus start(
        const EventProviderConfiguration& configuration) noexcept = 0;
    [[nodiscard]] virtual EventProviderPollResult poll(
        core::MonotonicTimePoint observed_at,
        std::span<core::SystemEvent> destination) noexcept = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual EventProviderCapabilities capabilities() const noexcept = 0;
};

} // namespace blackbox::telemetry
