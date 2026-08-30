#include "platform/macos/macos_global_hotkey_manager.hpp"

#include <catch2/catch_test_macros.hpp>

namespace macos_platform = blackbox::platform::macos;
namespace platform = blackbox::platform;

TEST_CASE("macOS shortcut matching preserves function key and modifiers",
          "[platform][macos][hotkey]") {
    const auto character = macos_platform::macos_function_character(
        platform::HotkeyKey::f12);
    REQUIRE(character != 0U);
    CHECK(macos_platform::matches_macos_hotkey(
        platform::default_incident_hotkey, character, true, true, false, false));
    CHECK_FALSE(macos_platform::matches_macos_hotkey(
        platform::default_incident_hotkey, character, true, false, false, false));
    CHECK_FALSE(macos_platform::matches_macos_hotkey(
        platform::default_incident_hotkey,
        macos_platform::macos_function_character(platform::HotkeyKey::f11),
        true, true, false, false));
}

TEST_CASE("macOS shortcut rejects invalid keys before permission access",
          "[platform][macos][hotkey]") {
    macos_platform::MacosGlobalHotkeyManager manager;
    auto invalid = platform::default_incident_hotkey;
    invalid.key = static_cast<platform::HotkeyKey>(0U);
    CHECK(manager.register_hotkey(invalid, [] {}) ==
          platform::HotkeyRegistrationResult::invalid_combination);
    CHECK_FALSE(manager.registered());
}
