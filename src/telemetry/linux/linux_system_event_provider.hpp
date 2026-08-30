#pragma once

#include "telemetry/event_provider.hpp"

#include <memory>
#include <string_view>

namespace blackbox::telemetry::linux {

[[nodiscard]] core::SystemEvent normalized_linux_sleep_event(bool sleeping) noexcept;
[[nodiscard]] core::SystemEvent normalized_linux_service_job_event(
    std::string_view result) noexcept;
[[nodiscard]] core::SystemEvent normalized_linux_application_crash_event() noexcept;

class LinuxSystemEventProvider final : public ISystemEventProvider {
public:
    LinuxSystemEventProvider() noexcept;
    ~LinuxSystemEventProvider() override;

    LinuxSystemEventProvider(const LinuxSystemEventProvider&) = delete;
    LinuxSystemEventProvider& operator=(const LinuxSystemEventProvider&) = delete;

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

} // namespace blackbox::telemetry::linux
