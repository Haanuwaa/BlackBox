#pragma once

#include "platform/global_hotkey.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace blackbox::platform::linux {

[[nodiscard]] std::string portal_accelerator(HotkeyCombination combination);

enum class PortalShortcutState : std::uint8_t {
    idle,
    active,
    reconnecting,
    unavailable,
};

enum class PortalShortcutEvent : std::uint8_t {
    session_established,
    session_lost,
    shortcut_removed,
    shortcut_restored,
    stop,
};

[[nodiscard]] constexpr PortalShortcutState portal_shortcut_transition(
    const PortalShortcutState current,
    const PortalShortcutEvent event) noexcept {
    switch (event) {
    case PortalShortcutEvent::session_established:
    case PortalShortcutEvent::shortcut_restored:
        return PortalShortcutState::active;
    case PortalShortcutEvent::session_lost:
        return current == PortalShortcutState::idle
            ? PortalShortcutState::idle : PortalShortcutState::reconnecting;
    case PortalShortcutEvent::shortcut_removed:
        return PortalShortcutState::unavailable;
    case PortalShortcutEvent::stop:
        return PortalShortcutState::idle;
    }
    return PortalShortcutState::unavailable;
}

[[nodiscard]] constexpr std::chrono::milliseconds portal_reconnect_delay(
    const std::uint32_t attempt) noexcept {
    constexpr std::uint32_t maximum_shift = 4U;
    const auto shift = attempt < maximum_shift ? attempt : maximum_shift;
    return std::chrono::milliseconds{250U << shift};
}

struct LinuxGlobalHotkeyDiagnostics {
    PortalShortcutState state{PortalShortcutState::idle};
    std::uint32_t portal_version{};
    std::uint64_t activations{};
    std::uint64_t shortcut_changes{};
    std::uint64_t session_losses{};
    std::uint64_t reconnect_attempts{};
    std::uint64_t reconnect_successes{};
};

class LinuxGlobalHotkeyManager final : public IGlobalHotkeyManager {
public:
    LinuxGlobalHotkeyManager();
    ~LinuxGlobalHotkeyManager() override;

    LinuxGlobalHotkeyManager(const LinuxGlobalHotkeyManager&) = delete;
    LinuxGlobalHotkeyManager& operator=(const LinuxGlobalHotkeyManager&) = delete;

    [[nodiscard]] HotkeyRegistrationResult register_hotkey(
        HotkeyCombination combination,
        HotkeyCallback callback) override;
    void unregister_hotkey() noexcept override;
    [[nodiscard]] bool registered() const noexcept override;
    [[nodiscard]] LinuxGlobalHotkeyDiagnostics diagnostics() const noexcept;

private:
    struct NativeState;
    std::unique_ptr<NativeState> native_{};
};

} // namespace blackbox::platform::linux
