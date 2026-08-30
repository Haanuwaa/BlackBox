#pragma once

#include <cstdint>
#include <functional>

namespace blackbox::platform {

enum class HotkeyKey : std::uint8_t {
    f1 = 1U,
    f2,
    f3,
    f4,
    f5,
    f6,
    f7,
    f8,
    f9,
    f10,
    f11,
    f12,
};

struct HotkeyCombination {
    HotkeyKey key{HotkeyKey::f12};
    bool control{true};
    bool shift{true};
    bool alt{};
    bool windows{};
    friend constexpr bool operator==(const HotkeyCombination&,
                                     const HotkeyCombination&) = default;
};

inline constexpr HotkeyCombination default_incident_hotkey{};

enum class HotkeyRegistrationResult : std::uint8_t {
    registered,
    conflict,
    permission_required,
    unavailable,
    invalid_combination,
};

using HotkeyCallback = std::function<void()>;

class IGlobalHotkeyManager {
public:
    virtual ~IGlobalHotkeyManager() = default;

    [[nodiscard]] virtual HotkeyRegistrationResult register_hotkey(
        HotkeyCombination combination,
        HotkeyCallback callback) = 0;
    virtual void unregister_hotkey() noexcept = 0;
    [[nodiscard]] virtual bool registered() const noexcept = 0;
};

} // namespace blackbox::platform
