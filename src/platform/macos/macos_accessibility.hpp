#pragma once

#include "platform/accessibility.hpp"

namespace blackbox::platform::macos {

[[nodiscard]] AccessibilityPreferences accessibility_preferences() noexcept;

} // namespace blackbox::platform::macos
