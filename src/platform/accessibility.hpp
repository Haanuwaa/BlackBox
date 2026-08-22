#pragma once

namespace blackbox::platform {

struct AccessibilityPreferences {
    bool high_contrast{};
    bool animations_enabled{true};
    friend constexpr bool operator==(const AccessibilityPreferences&,
                                     const AccessibilityPreferences&) = default;
};

} // namespace blackbox::platform
