#pragma once

#include "platform/global_hotkey.hpp"

#include <memory>

namespace blackbox::platform::windows {

class WindowsGlobalHotkeyManager final : public IGlobalHotkeyManager {
public:
    WindowsGlobalHotkeyManager();
    ~WindowsGlobalHotkeyManager() override;

    WindowsGlobalHotkeyManager(const WindowsGlobalHotkeyManager&) = delete;
    WindowsGlobalHotkeyManager& operator=(const WindowsGlobalHotkeyManager&) = delete;

    [[nodiscard]] HotkeyRegistrationResult register_hotkey(
        HotkeyCombination combination,
        HotkeyCallback callback) override;
    void unregister_hotkey() noexcept override;
    [[nodiscard]] bool registered() const noexcept override;

    // Integration seam: posts the same thread message produced by Windows
    // after a registered chord, without synthesizing user keyboard input.
    [[nodiscard]] bool post_activation_for_testing() noexcept;

private:
    struct NativeState;
    std::unique_ptr<NativeState> native_;
};

} // namespace blackbox::platform::windows
