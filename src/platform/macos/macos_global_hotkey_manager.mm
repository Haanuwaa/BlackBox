#include "platform/macos/macos_global_hotkey_manager.hpp"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CGEvent.h>

#include <utility>

namespace blackbox::platform::macos {

std::uint32_t macos_function_character(const HotkeyKey key) noexcept {
    switch (key) {
    case HotkeyKey::f1: return NSF1FunctionKey;
    case HotkeyKey::f2: return NSF2FunctionKey;
    case HotkeyKey::f3: return NSF3FunctionKey;
    case HotkeyKey::f4: return NSF4FunctionKey;
    case HotkeyKey::f5: return NSF5FunctionKey;
    case HotkeyKey::f6: return NSF6FunctionKey;
    case HotkeyKey::f7: return NSF7FunctionKey;
    case HotkeyKey::f8: return NSF8FunctionKey;
    case HotkeyKey::f9: return NSF9FunctionKey;
    case HotkeyKey::f10: return NSF10FunctionKey;
    case HotkeyKey::f11: return NSF11FunctionKey;
    case HotkeyKey::f12: return NSF12FunctionKey;
    }
    return 0U;
}

bool matches_macos_hotkey(const HotkeyCombination combination,
                          const std::uint32_t function_character,
                          const bool control,
                          const bool shift,
                          const bool option,
                          const bool command) noexcept {
    const auto expected = macos_function_character(combination.key);
    return expected != 0U && function_character == expected &&
           combination.control == control && combination.shift == shift &&
           combination.alt == option && combination.windows == command;
}

struct MacosGlobalHotkeyManager::NativeState {
    [[nodiscard]] bool matches(NSEvent* event) const noexcept {
        if (event == nil || event.isARepeat) return false;
        NSString* characters = event.charactersIgnoringModifiers;
        if (characters == nil || characters.length != 1U) return false;
        const auto flags = event.modifierFlags;
        return matches_macos_hotkey(
            combination, [characters characterAtIndex:0],
            (flags & NSEventModifierFlagControl) != 0U,
            (flags & NSEventModifierFlagShift) != 0U,
            (flags & NSEventModifierFlagOption) != 0U,
            (flags & NSEventModifierFlagCommand) != 0U);
    }

    void activate(NSEvent* event) noexcept {
        if (!matches(event) || !callback) return;
        try {
            callback();
        } catch (...) {
            // AppKit event handlers must not propagate application failures.
        }
    }

    HotkeyCombination combination{};
    HotkeyCallback callback{};
    id global_monitor{nil};
    id local_monitor{nil};
};

MacosGlobalHotkeyManager::MacosGlobalHotkeyManager()
    : native_{std::make_unique<NativeState>()} {}

MacosGlobalHotkeyManager::~MacosGlobalHotkeyManager() {
    unregister_hotkey();
}

HotkeyRegistrationResult MacosGlobalHotkeyManager::register_hotkey(
    const HotkeyCombination combination,
    HotkeyCallback callback) {
    unregister_hotkey();
    if (macos_function_character(combination.key) == 0U || !callback) {
        return HotkeyRegistrationResult::invalid_combination;
    }

    if (!CGPreflightListenEventAccess() && !CGRequestListenEventAccess()) {
        return HotkeyRegistrationResult::permission_required;
    }

    @autoreleasepool {
        native_->combination = combination;
        native_->callback = std::move(callback);
        auto* state = native_.get();
        native_->global_monitor = [NSEvent
            addGlobalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                         handler:^(NSEvent* event) {
                                             state->activate(event);
                                         }];
        native_->local_monitor = [NSEvent
            addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                        handler:^NSEvent*(NSEvent* event) {
                                            state->activate(event);
                                            return event;
                                        }];
        if (native_->global_monitor == nil || native_->local_monitor == nil) {
            unregister_hotkey();
            return HotkeyRegistrationResult::unavailable;
        }
    }
    return HotkeyRegistrationResult::registered;
}

void MacosGlobalHotkeyManager::unregister_hotkey() noexcept {
    if (native_ == nullptr) return;
    @autoreleasepool {
        if (native_->global_monitor != nil) {
            [NSEvent removeMonitor:native_->global_monitor];
            native_->global_monitor = nil;
        }
        if (native_->local_monitor != nil) {
            [NSEvent removeMonitor:native_->local_monitor];
            native_->local_monitor = nil;
        }
        native_->callback = {};
    }
}

bool MacosGlobalHotkeyManager::registered() const noexcept {
    return native_ != nullptr && native_->global_monitor != nil &&
           native_->local_monitor != nil;
}

} // namespace blackbox::platform::macos
