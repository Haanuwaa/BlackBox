#include "ui/product_ui_model.hpp"

#include <algorithm>
#include <cmath>
#include <imgui.h>

namespace blackbox::ui {

float scale_for_dpi(const float dpi) noexcept {
    if (!std::isfinite(dpi) || dpi <= 0.0F) return 1.0F;
    return std::clamp(dpi / 96.0F, 0.75F, 3.0F);
}

float scaled_ui_metric(const float logical_pixels, const float dpi) noexcept {
    if (!std::isfinite(logical_pixels) || logical_pixels < 0.0F) return 0.0F;
    return logical_pixels * scale_for_dpi(dpi);
}

std::size_t choose_display_work_area(
    const int window_x, const int window_y, const int window_width,
    const int window_height, const DisplayWorkArea* displays,
    const std::size_t display_count) noexcept {
    if (displays == nullptr || display_count == 0U) return 0U;
    std::size_t best{};
    std::int64_t best_overlap{-1};
    for (std::size_t index = 0U; index < display_count; ++index) {
        const auto& display = displays[index];
        const auto left = std::max(window_x, display.x);
        const auto top = std::max(window_y, display.y);
        const auto right = std::min(window_x + std::max(0, window_width),
                                    display.x + std::max(0, display.width));
        const auto bottom = std::min(window_y + std::max(0, window_height),
                                     display.y + std::max(0, display.height));
        const auto overlap = static_cast<std::int64_t>(std::max(0, right - left)) *
                             static_cast<std::int64_t>(std::max(0, bottom - top));
        if (overlap > best_overlap) { best_overlap = overlap; best = index; }
    }
    return best;
}

AccessibilityPalette accessibility_palette(const bool high_contrast,
                                            const bool dark_theme) noexcept {
    if (high_contrast) {
        return {{0.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F},
                {0.0F, 1.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 0.0F, 1.0F}};
    }
    if (dark_theme) {
        return {{0.07F, 0.08F, 0.10F, 1.0F}, {0.92F, 0.94F, 0.97F, 1.0F},
                {0.20F, 0.70F, 1.0F, 1.0F}, {1.0F, 0.65F, 0.20F, 1.0F}};
    }
    return {{0.95F, 0.96F, 0.98F, 1.0F}, {0.08F, 0.10F, 0.14F, 1.0F},
            {0.05F, 0.35F, 0.75F, 1.0F}, {0.70F, 0.30F, 0.0F, 1.0F}};
}

void apply_accessibility_style(const bool high_contrast) noexcept {
    ImGui::StyleColorsDark();
    if (!high_contrast) return;

    const auto palette = accessibility_palette(true, true);
    const auto color = [](const std::array<float, 4U>& value) noexcept {
        return ImVec4{value[0], value[1], value[2], value[3]};
    };
    const auto background = color(palette.background);
    const auto foreground = color(palette.foreground);
    const auto accent = color(palette.accent);
    constexpr ImVec4 disabled{0.75F, 0.75F, 0.75F, 1.0F};
    constexpr ImVec4 control{0.0F, 0.25F, 0.25F, 1.0F};
    constexpr ImVec4 control_hovered{0.0F, 0.55F, 0.55F, 1.0F};
    constexpr ImVec4 control_active{0.0F, 0.75F, 0.75F, 1.0F};

    auto& colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_Text] = foreground;
    colors[ImGuiCol_TextDisabled] = disabled;
    colors[ImGuiCol_WindowBg] = background;
    colors[ImGuiCol_ChildBg] = background;
    colors[ImGuiCol_PopupBg] = background;
    colors[ImGuiCol_Border] = foreground;
    colors[ImGuiCol_BorderShadow] = background;
    colors[ImGuiCol_FrameBg] = control;
    colors[ImGuiCol_FrameBgHovered] = control_hovered;
    colors[ImGuiCol_FrameBgActive] = control_active;
    colors[ImGuiCol_TitleBg] = background;
    colors[ImGuiCol_TitleBgActive] = control;
    colors[ImGuiCol_TitleBgCollapsed] = background;
    colors[ImGuiCol_MenuBarBg] = background;
    colors[ImGuiCol_ScrollbarBg] = background;
    colors[ImGuiCol_ScrollbarGrab] = control;
    colors[ImGuiCol_ScrollbarGrabHovered] = control_hovered;
    colors[ImGuiCol_ScrollbarGrabActive] = control_active;
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = foreground;
    colors[ImGuiCol_Button] = control;
    colors[ImGuiCol_ButtonHovered] = control_hovered;
    colors[ImGuiCol_ButtonActive] = control_active;
    colors[ImGuiCol_Header] = control;
    colors[ImGuiCol_HeaderHovered] = control_hovered;
    colors[ImGuiCol_HeaderActive] = control_active;
    colors[ImGuiCol_Separator] = foreground;
    colors[ImGuiCol_SeparatorHovered] = accent;
    colors[ImGuiCol_SeparatorActive] = foreground;
    colors[ImGuiCol_TableHeaderBg] = control;
    colors[ImGuiCol_TableBorderStrong] = foreground;
    colors[ImGuiCol_TableBorderLight] = disabled;
    colors[ImGuiCol_TextSelectedBg] = control_hovered;
}

bool update_accessibility_style(bool& current_high_contrast,
                                const bool requested_high_contrast) noexcept {
    if (current_high_contrast == requested_high_contrast) return false;
    current_high_contrast = requested_high_contrast;
    apply_accessibility_style(current_high_contrast);
    return true;
}

} // namespace blackbox::ui
