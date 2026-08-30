#pragma once

#include "telemetry/event_provider.hpp"

#include <memory>

namespace blackbox::telemetry::macos {

[[nodiscard]] core::SystemEvent normalized_macos_media_event(bool added) noexcept;
[[nodiscard]] core::SystemEvent normalized_macos_sleep_event(bool sleeping) noexcept;

class MacosSystemEventProvider final : public ISystemEventProvider {
public:
    MacosSystemEventProvider() noexcept;
    ~MacosSystemEventProvider() override;

    MacosSystemEventProvider(const MacosSystemEventProvider&) = delete;
    MacosSystemEventProvider& operator=(const MacosSystemEventProvider&) = delete;

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

} // namespace blackbox::telemetry::macos
