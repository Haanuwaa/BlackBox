#include "platform/linux/linux_global_hotkey_manager.hpp"

#include <catch2/catch_test_macros.hpp>

namespace linux_platform = blackbox::platform::linux;
namespace platform = blackbox::platform;

TEST_CASE("Linux portal accelerator preserves the configured combination",
          "[platform][linux][hotkey][portal]") {
    CHECK(linux_platform::portal_accelerator(
              platform::default_incident_hotkey) == "CTRL+SHIFT+F12");
    CHECK(linux_platform::portal_accelerator(
              platform::HotkeyCombination{platform::HotkeyKey::f3,
                                          false, false, true, true}) ==
          "ALT+LOGO+F3");
}

TEST_CASE("Linux portal hotkey rejects invalid keys before desktop access",
          "[platform][linux][hotkey][portal]") {
    linux_platform::LinuxGlobalHotkeyManager manager;
    auto invalid = platform::default_incident_hotkey;
    invalid.key = static_cast<platform::HotkeyKey>(0U);
    CHECK(manager.register_hotkey(invalid, [] {}) ==
          platform::HotkeyRegistrationResult::invalid_combination);
    CHECK_FALSE(manager.registered());
}
