#include "platform/macos/macos_accessibility.hpp"

#import <AppKit/AppKit.h>

namespace blackbox::platform::macos {

AccessibilityPreferences accessibility_preferences() noexcept {
    @autoreleasepool {
        const auto* workspace = [NSWorkspace sharedWorkspace];
        return AccessibilityPreferences{
            .high_contrast =
                workspace.accessibilityDisplayShouldIncreaseContrast != NO,
            .animations_enabled =
                workspace.accessibilityDisplayShouldReduceMotion == NO,
        };
    }
}

} // namespace blackbox::platform::macos
