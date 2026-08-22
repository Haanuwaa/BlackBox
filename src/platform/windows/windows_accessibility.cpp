#include "platform/windows/windows_accessibility.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace blackbox::platform::windows {

AccessibilityPreferences accessibility_preferences() noexcept {
    AccessibilityPreferences result{};
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast),
                              &contrast, 0U) != FALSE) {
        result.high_contrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0U;
    }
    BOOL animations = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0U,
                              &animations, 0U) != FALSE) {
        result.animations_enabled = animations != FALSE;
    }
    return result;
}

} // namespace blackbox::platform::windows
