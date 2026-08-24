#pragma once

#include "platform/accessibility.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace blackbox::platform::linux {

struct LinuxAccessibilitySnapshot {
  AccessibilityPreferences preferences{};
  bool portal_available{};
  bool contrast_available{};
  bool reduced_motion_available{};
  std::uint64_t refreshes_completed{};
  std::uint64_t refresh_failures{};
  friend constexpr bool
  operator==(const LinuxAccessibilitySnapshot &,
             const LinuxAccessibilitySnapshot &) = default;
};

[[nodiscard]] constexpr AccessibilityPreferences
portal_accessibility_preferences(
    const std::optional<std::uint32_t> contrast,
    const std::optional<std::uint32_t> reduced_motion) noexcept {
  return AccessibilityPreferences{
      .high_contrast = contrast.value_or(0U) == 1U,
      .animations_enabled = reduced_motion.value_or(0U) != 1U,
  };
}

// Owns the potentially blocking session-bus work on one bounded worker. The
// application requests refreshes only while visible; requests coalesce and
// cached preferences are read without a D-Bus call on the render thread.
class LinuxAccessibilityMonitor final {
public:
  LinuxAccessibilityMonitor() noexcept;
  ~LinuxAccessibilityMonitor();

  LinuxAccessibilityMonitor(const LinuxAccessibilityMonitor &) = delete;
  LinuxAccessibilityMonitor &
  operator=(const LinuxAccessibilityMonitor &) = delete;
  LinuxAccessibilityMonitor(LinuxAccessibilityMonitor &&) = delete;
  LinuxAccessibilityMonitor &operator=(LinuxAccessibilityMonitor &&) = delete;

  [[nodiscard]] bool start() noexcept;
  void request_refresh() noexcept;
  void stop() noexcept;
  [[nodiscard]] LinuxAccessibilitySnapshot snapshot() const noexcept;

private:
  struct NativeState;
  std::unique_ptr<NativeState> native_state_{};
};

} // namespace blackbox::platform::linux
