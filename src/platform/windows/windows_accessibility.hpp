#pragma once

#include "platform/accessibility.hpp"

namespace blackbox::platform::windows {

[[nodiscard]] AccessibilityPreferences accessibility_preferences() noexcept;

} // namespace blackbox::platform::windows
