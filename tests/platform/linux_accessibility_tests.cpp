#include "platform/linux/linux_accessibility.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

namespace linux_platform = blackbox::platform::linux;

TEST_CASE("Linux portal accessibility values follow the XDG value domains",
          "[platform][linux][accessibility]") {
  CHECK(linux_platform::portal_accessibility_preferences(1U, 1U) ==
        blackbox::platform::AccessibilityPreferences{
            .high_contrast = true, .animations_enabled = false});
  CHECK(linux_platform::portal_accessibility_preferences(0U, 0U) ==
        blackbox::platform::AccessibilityPreferences{
            .high_contrast = false, .animations_enabled = true});
  CHECK(linux_platform::portal_accessibility_preferences(7U, 9U) ==
        blackbox::platform::AccessibilityPreferences{
            .high_contrast = false, .animations_enabled = true});
  CHECK(linux_platform::portal_accessibility_preferences(std::nullopt,
                                                         std::nullopt) ==
        blackbox::platform::AccessibilityPreferences{});
}

TEST_CASE("Linux accessibility refresh stays off the caller thread",
          "[platform][linux][accessibility][integration]") {
  linux_platform::LinuxAccessibilityMonitor monitor;
  REQUIRE(monitor.start());
  monitor.request_refresh();

  for (unsigned attempt = 0U;
       attempt < 150U && monitor.snapshot().refreshes_completed == 0U;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  CHECK(monitor.snapshot().refreshes_completed == 1U);
  monitor.stop();
}
