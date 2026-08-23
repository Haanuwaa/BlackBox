#pragma once

#include "platform/global_hotkey.hpp"

#include <memory>
#include <string>

namespace blackbox::platform::linux {

[[nodiscard]] std::string portal_accelerator(HotkeyCombination combination);

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

private:
    struct NativeState;
    std::unique_ptr<NativeState> native_{};
};

} // namespace blackbox::platform::linux
