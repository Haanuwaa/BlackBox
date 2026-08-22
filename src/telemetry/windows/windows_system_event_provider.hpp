#pragma once

#include "telemetry/event_provider.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace blackbox::telemetry::windows {

// Portable fixture boundary for the privacy-normalized Windows Event Log
// allowlist. Native messages and event data never cross this API.
[[nodiscard]] std::optional<core::SystemEventKind> normalized_windows_event_kind(
    core::SystemEventSource source, std::uint32_t native_event_id) noexcept;

class WindowsSystemEventProvider final : public ISystemEventProvider {
public:
    WindowsSystemEventProvider() noexcept;
    ~WindowsSystemEventProvider() override;

    WindowsSystemEventProvider(const WindowsSystemEventProvider&) = delete;
    WindowsSystemEventProvider& operator=(const WindowsSystemEventProvider&) = delete;

    [[nodiscard]] EventProviderStatus start(
        const EventProviderConfiguration& configuration) noexcept override;
    [[nodiscard]] EventProviderPollResult poll(
        core::MonotonicTimePoint observed_at,
        std::span<core::SystemEvent> destination) noexcept override;
    void stop() noexcept override;
    [[nodiscard]] EventProviderCapabilities capabilities() const noexcept override;

private:
    struct NativeState;
    std::unique_ptr<NativeState> state_{};
};

} // namespace blackbox::telemetry::windows
