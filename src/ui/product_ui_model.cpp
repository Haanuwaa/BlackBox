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

OnboardingLayout onboarding_layout(const float viewport_width,
                                    const float viewport_height) noexcept {
    const auto finite_width = std::isfinite(viewport_width) ? viewport_width : 0.0F;
    const auto finite_height = std::isfinite(viewport_height) ? viewport_height : 0.0F;
    const auto available_width = std::max(240.0F, finite_width - 32.0F);
    const auto available_height = std::max(240.0F, finite_height - 32.0F);
    return {std::min(640.0F, available_width),
            std::min(560.0F, available_height),
            available_width < 560.0F || available_height < 480.0F};
}

std::size_t navigation_column_count(const float available_width) noexcept {
    if (!std::isfinite(available_width) || available_width <= 0.0F) return 2U;
    if (available_width < 560.0F) return 2U;
    if (available_width < 860.0F) return 3U;
    return 6U;
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

ProductVisualStyle product_visual_style(const bool high_contrast) noexcept {
    if (high_contrast) {
        return {{0.0F, 0.0F, 0.0F, 1.0F},
                {0.0F, 0.0F, 0.0F, 1.0F},
                {0.0F, 0.18F, 0.18F, 1.0F},
                {1.0F, 1.0F, 1.0F, 1.0F},
                {0.78F, 0.78F, 0.78F, 1.0F},
                {0.0F, 1.0F, 1.0F, 1.0F},
                {0.0F, 0.65F, 0.65F, 1.0F},
                {0.25F, 1.0F, 0.45F, 1.0F},
                {1.0F, 1.0F, 0.0F, 1.0F},
                {1.0F, 1.0F, 1.0F, 1.0F},
                0.0F, 0.0F, 0.0F};
    }
    return {{0.035F, 0.043F, 0.060F, 1.0F},
            {0.060F, 0.075F, 0.102F, 1.0F},
            {0.085F, 0.105F, 0.142F, 1.0F},
            {0.925F, 0.945F, 0.975F, 1.0F},
            {0.590F, 0.650F, 0.735F, 1.0F},
            {0.180F, 0.600F, 1.0F, 1.0F},
            {0.250F, 0.680F, 1.0F, 1.0F},
            {0.250F, 0.820F, 0.540F, 1.0F},
            {1.0F, 0.650F, 0.220F, 1.0F},
            {0.150F, 0.190F, 0.260F, 1.0F},
            8.0F, 7.0F, 5.0F};
}

IncidentArchivePresentation incident_archive_presentation(
    const IncidentViewerLoadState state, const std::uint64_t total_matching,
    const bool search_active) noexcept {
    switch (state) {
    case IncidentViewerLoadState::disabled:
    case IncidentViewerLoadState::error:
        return IncidentArchivePresentation::unavailable;
    case IncidentViewerLoadState::idle:
    case IncidentViewerLoadState::loading:
        return IncidentArchivePresentation::loading;
    case IncidentViewerLoadState::ready:
        if (total_matching != 0U) return IncidentArchivePresentation::results;
        return search_active ? IncidentArchivePresentation::no_matches
                             : IncidentArchivePresentation::empty;
    }
    return IncidentArchivePresentation::unavailable;
}

void apply_accessibility_style(const bool high_contrast) noexcept {
    ImGui::StyleColorsDark();
    const auto color = [](const std::array<float, 4U>& value) noexcept {
        return ImVec4{value[0], value[1], value[2], value[3]};
    };
    const auto visual = product_visual_style(high_contrast);
    auto& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2{16.0F, 14.0F};
    style.FramePadding = ImVec2{10.0F, 6.0F};
    style.CellPadding = ImVec2{8.0F, 6.0F};
    style.ItemSpacing = ImVec2{10.0F, 8.0F};
    style.ItemInnerSpacing = ImVec2{7.0F, 5.0F};
    style.IndentSpacing = 22.0F;
    style.ScrollbarSize = 15.0F;
    style.GrabMinSize = 11.0F;
    style.WindowRounding = visual.window_rounding;
    style.ChildRounding = visual.child_rounding;
    style.FrameRounding = visual.frame_rounding;
    style.PopupRounding = visual.child_rounding;
    style.ScrollbarRounding = visual.frame_rounding;
    style.GrabRounding = visual.frame_rounding;
    style.TabRounding = visual.frame_rounding;
    style.WindowBorderSize = high_contrast ? 1.0F : 0.0F;
    style.ChildBorderSize = 1.0F;
    style.FrameBorderSize = high_contrast ? 1.0F : 0.0F;

    auto& colors = style.Colors;
    const auto background = color(visual.background);
    const auto surface = color(visual.surface);
    const auto raised = color(visual.surface_raised);
    const auto foreground = color(visual.foreground);
    const auto muted = color(visual.muted);
    const auto accent = color(visual.accent);
    const auto accent_hovered = color(visual.accent_hovered);
    const auto border = color(visual.border);
    colors[ImGuiCol_Text] = foreground;
    colors[ImGuiCol_TextDisabled] = muted;
    colors[ImGuiCol_WindowBg] = background;
    colors[ImGuiCol_ChildBg] = surface;
    colors[ImGuiCol_PopupBg] = surface;
    colors[ImGuiCol_Border] = high_contrast ? foreground : border;
    colors[ImGuiCol_BorderShadow] = background;
    colors[ImGuiCol_FrameBg] = raised;
    colors[ImGuiCol_FrameBgHovered] = accent_hovered;
    colors[ImGuiCol_FrameBgActive] = accent;
    colors[ImGuiCol_TitleBg] = background;
    colors[ImGuiCol_TitleBgActive] = surface;
    colors[ImGuiCol_TitleBgCollapsed] = background;
    colors[ImGuiCol_MenuBarBg] = surface;
    colors[ImGuiCol_ScrollbarBg] = background;
    colors[ImGuiCol_ScrollbarGrab] = raised;
    colors[ImGuiCol_ScrollbarGrabHovered] = accent_hovered;
    colors[ImGuiCol_ScrollbarGrabActive] = accent;
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = foreground;
    colors[ImGuiCol_Button] = raised;
    colors[ImGuiCol_ButtonHovered] = accent_hovered;
    colors[ImGuiCol_ButtonActive] = accent;
    colors[ImGuiCol_Header] = raised;
    colors[ImGuiCol_HeaderHovered] = accent_hovered;
    colors[ImGuiCol_HeaderActive] = accent;
    colors[ImGuiCol_Separator] = high_contrast ? foreground : border;
    colors[ImGuiCol_SeparatorHovered] = accent;
    colors[ImGuiCol_SeparatorActive] = foreground;
    colors[ImGuiCol_TableHeaderBg] = raised;
    colors[ImGuiCol_TableBorderStrong] = high_contrast ? foreground : border;
    colors[ImGuiCol_TableBorderLight] = border;
    colors[ImGuiCol_TableRowBgAlt] = ImVec4{raised.x, raised.y, raised.z, 0.38F};
    colors[ImGuiCol_TextSelectedBg] = accent_hovered;
    colors[ImGuiCol_NavCursor] = accent;
}

bool update_accessibility_style(bool& current_high_contrast,
                                const bool requested_high_contrast) noexcept {
    if (current_high_contrast == requested_high_contrast) return false;
    current_high_contrast = requested_high_contrast;
    apply_accessibility_style(current_high_contrast);
    return true;
}

} // namespace blackbox::ui
