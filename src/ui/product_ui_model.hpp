#pragma once

#include "ui/dashboard.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace blackbox::ui {

struct DisplayWorkArea {
    int x{}; int y{}; int width{}; int height{};
    friend constexpr bool operator==(const DisplayWorkArea&,
                                     const DisplayWorkArea&) = default;
};

struct AccessibilityPalette {
    std::array<float, 4U> background{};
    std::array<float, 4U> foreground{};
    std::array<float, 4U> accent{};
    std::array<float, 4U> warning{};
    friend constexpr bool operator==(const AccessibilityPalette&,
                                     const AccessibilityPalette&) = default;
};

enum class IncidentArchivePresentation : std::uint8_t {
    loading,
    empty,
    no_matches,
    results,
    unavailable,
};

[[nodiscard]] constexpr ProductPage page_for_keyboard_shortcut(
    ProductPage current, bool control, unsigned digit) noexcept {
    return control && digit >= 1U && digit <= 6U
        ? static_cast<ProductPage>(digit - 1U) : current;
}
[[nodiscard]] float scale_for_dpi(float dpi) noexcept;
[[nodiscard]] float scaled_ui_metric(float logical_pixels, float dpi) noexcept;
[[nodiscard]] std::size_t choose_display_work_area(
    int window_x, int window_y, int window_width, int window_height,
    const DisplayWorkArea* displays, std::size_t display_count) noexcept;
[[nodiscard]] AccessibilityPalette accessibility_palette(bool high_contrast,
                                                          bool dark_theme) noexcept;
[[nodiscard]] IncidentArchivePresentation incident_archive_presentation(
    IncidentViewerLoadState state, std::uint64_t total_matching,
    bool search_active) noexcept;
void apply_accessibility_style(bool high_contrast) noexcept;
[[nodiscard]] bool update_accessibility_style(bool& current_high_contrast,
                                              bool requested_high_contrast) noexcept;

} // namespace blackbox::ui
