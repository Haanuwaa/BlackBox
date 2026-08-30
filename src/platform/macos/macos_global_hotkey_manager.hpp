#pragma once

#include "platform/global_hotkey.hpp"

#include <cstdint>
#include <memory>

namespace blackbox::platform::macos {

[[nodiscard]] std::uint32_t macos_function_character(HotkeyKey key) noexcept;
[[nodiscard]] bool matches_macos_hotkey(HotkeyCombination combination,
                                        std::uint32_t function_character,
                                        bool control,
                                        bool shift,
                                        bool option,
                                        bool command) noexcept;

class MacosGlobalHotkeyManager final : public IGlobalHotkeyManager {
public:
    MacosGlobalHotkeyManager();
    ~MacosGlobalHotkeyManager() override;

    MacosGlobalHotkeyManager(const MacosGlobalHotkeyManager&) = delete;
    MacosGlobalHotkeyManager& operator=(const MacosGlobalHotkeyManager&) = delete;

    [[nodiscard]] HotkeyRegistrationResult register_hotkey(
        HotkeyCombination combination,
        HotkeyCallback callback) override;
    void unregister_hotkey() noexcept override;
    [[nodiscard]] bool registered() const noexcept override;

private:
    struct NativeState;
    std::unique_ptr<NativeState> native_{};
};

} // namespace blackbox::platform::macos
