#include "ui/product_ui_model.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <iterator>
#include <limits>
#include <memory>
#include <string>

namespace ui = blackbox::ui;

namespace {

class ScopedImGuiContext final {
public:
    ScopedImGuiContext() { ImGui::CreateContext(); }
    ~ScopedImGuiContext() { ImGui::DestroyContext(); }
    ScopedImGuiContext(const ScopedImGuiContext&) = delete;
    ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;
};

void check_color(const ImVec4& actual, const std::array<float, 4U>& expected) {
    CHECK(actual.x == expected[0]);
    CHECK(actual.y == expected[1]);
    CHECK(actual.z == expected[2]);
    CHECK(actual.w == expected[3]);
}

} // namespace

TEST_CASE("incident timelines share one bounded marker-relative cursor",
          "[ui][interaction][timeline][cursor]") {
    ui::ProductUiState product{};
    CHECK_FALSE(product.timeline_cursor_visible);

    REQUIRE(ui::set_timeline_cursor(product, -12.5, -120.0, 30.0));
    CHECK(product.timeline_cursor_visible);
    CHECK(product.timeline_cursor_seconds == -12.5);

    REQUIRE(ui::set_timeline_cursor(product, 80.0, -120.0, 30.0));
    CHECK(product.timeline_cursor_seconds == 30.0);
    REQUIRE(ui::set_timeline_cursor(product, -500.0, -120.0, 30.0));
    CHECK(product.timeline_cursor_seconds == -120.0);

    CHECK_FALSE(ui::set_timeline_cursor(
        product, std::numeric_limits<double>::quiet_NaN(), -120.0, 30.0));
    CHECK_FALSE(ui::set_timeline_cursor(product, 0.0, 30.0, -120.0));
    CHECK(product.timeline_cursor_seconds == -120.0);

    ui::clear_timeline_cursor(product);
    CHECK_FALSE(product.timeline_cursor_visible);
    CHECK(product.timeline_cursor_seconds == 0.0);
}

TEST_CASE("product pages support deterministic keyboard navigation",
          "[ui][interaction][keyboard][navigation]") {
    CHECK(ui::page_for_keyboard_shortcut(ui::ProductPage::live, true, 1U) == ui::ProductPage::live);
    CHECK(ui::page_for_keyboard_shortcut(ui::ProductPage::live, true, 6U) == ui::ProductPage::diagnostics);
    CHECK(ui::page_for_keyboard_shortcut(ui::ProductPage::patterns, false, 2U) == ui::ProductPage::patterns);
    CHECK(ui::page_for_keyboard_shortcut(ui::ProductPage::detail, true, 0U) == ui::ProductPage::detail);
}

TEST_CASE("incident archive presentation distinguishes empty search and failure states",
          "[ui][interaction][empty-state]") {
    using Archive = ui::IncidentArchivePresentation;
    using Load = ui::IncidentViewerLoadState;

    CHECK(ui::incident_archive_presentation(Load::idle, 0U, false) == Archive::loading);
    CHECK(ui::incident_archive_presentation(Load::loading, 0U, false) == Archive::loading);
    CHECK(ui::incident_archive_presentation(Load::ready, 0U, false) == Archive::empty);
    CHECK(ui::incident_archive_presentation(Load::ready, 0U, true) == Archive::no_matches);
    CHECK(ui::incident_archive_presentation(Load::ready, 3U, false) == Archive::results);
    CHECK(ui::incident_archive_presentation(Load::disabled, 0U, false) == Archive::unavailable);
    CHECK(ui::incident_archive_presentation(Load::error, 0U, true) == Archive::unavailable);
}

TEST_CASE("first-run onboarding renders as a blocking guided start",
          "[ui][interaction][onboarding][render]") {
    const ScopedImGuiContext context;
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2{1'100.0F, 700.0F};
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    unsigned char* font_pixels{};
    int font_width{};
    int font_height{};
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
    REQUIRE(font_pixels != nullptr);

    auto dashboard = std::make_unique<ui::DashboardState>();
    auto viewer = std::make_unique<ui::IncidentViewerState>();
    auto product = std::make_unique<ui::ProductUiState>();
    ImGui::NewFrame();
    const auto command = ui::render_dashboard(*dashboard, *viewer, *product);
    CHECK(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId));
    CHECK(product->onboarding_open);
    CHECK(command.action == ui::DashboardAction::none);
    ImGui::Render();
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
}

TEST_CASE("rendered empty incident archive remains navigable",
          "[ui][interaction][empty-state][render]") {
    const ScopedImGuiContext context;
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2{1'100.0F, 700.0F};
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    unsigned char* font_pixels{};
    int font_width{};
    int font_height{};
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
    REQUIRE(font_pixels != nullptr);

    auto content = std::make_shared<ui::IncidentViewerContent>();
    content->state = ui::IncidentViewerLoadState::ready;
    content->status = "No saved incidents";
    auto viewer = std::make_unique<ui::IncidentViewerState>();
    viewer->content = std::move(content);
    auto dashboard = std::make_unique<ui::DashboardState>();
    auto product = std::make_unique<ui::ProductUiState>();
    product->onboarding_open = false;
    product->page = ui::ProductPage::incidents;
    ImGui::NewFrame();
    const auto command = ui::render_dashboard(*dashboard, *viewer, *product);
    CHECK(product->page == ui::ProductPage::incidents);
    CHECK(command.action == ui::DashboardAction::none);
    ImGui::Render();
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
}

TEST_CASE("rendered product navigation accepts the documented Ctrl digit shortcuts",
          "[ui][interaction][keyboard][navigation][render]") {
    const ScopedImGuiContext context;
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.DisplaySize = ImVec2{1'100.0F, 700.0F};
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    unsigned char* font_pixels{};
    int font_width{};
    int font_height{};
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
    REQUIRE(font_pixels != nullptr);
    REQUIRE(font_width > 0);
    REQUIRE(font_height > 0);
    auto dashboard = std::make_unique<ui::DashboardState>();
    auto viewer = std::make_unique<ui::IncidentViewerState>();
    auto product = std::make_unique<ui::ProductUiState>();
    product->onboarding_open = false;
    product->page = ui::ProductPage::live;

    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_6, true);
    ImGui::NewFrame();
    static_cast<void>(ui::render_dashboard(*dashboard, *viewer, *product));
    CHECK(product->page == ui::ProductPage::diagnostics);
    ImGui::Render();

    io.AddKeyEvent(ImGuiKey_6, false);
    io.AddKeyEvent(ImGuiKey_1, true);
    ImGui::NewFrame();
    static_cast<void>(ui::render_dashboard(*dashboard, *viewer, *product));
    CHECK(product->page == ui::ProductPage::live);
    ImGui::Render();
}

TEST_CASE("DPI metrics remain bounded and proportional", "[ui][dpi][layout]") {
    CHECK(ui::scale_for_dpi(96.0F) == 1.0F);
    CHECK(ui::scale_for_dpi(192.0F) == 2.0F);
    CHECK(ui::scale_for_dpi(20.0F) == 0.75F);
    CHECK(ui::scale_for_dpi(1'000.0F) == 3.0F);
    CHECK(ui::scaled_ui_metric(44.0F, 144.0F) == 66.0F);
}

TEST_CASE("onboarding remains bounded on compact and scaled work areas",
          "[ui][dpi][accessibility][onboarding][layout]") {
    const auto standard = ui::onboarding_layout(1'100.0F, 700.0F);
    CHECK(standard.width == 640.0F);
    CHECK(standard.height == 560.0F);
    CHECK_FALSE(standard.compact);

    const auto compact = ui::onboarding_layout(360.0F, 420.0F);
    CHECK(compact.width == 328.0F);
    CHECK(compact.height == 388.0F);
    CHECK(compact.compact);

    const auto invalid = ui::onboarding_layout(
        std::numeric_limits<float>::quiet_NaN(), -1.0F);
    CHECK(invalid.width == 240.0F);
    CHECK(invalid.height == 240.0F);
    CHECK(invalid.compact);
}

TEST_CASE("multi-monitor layout selects the greatest visible work area", "[ui][multi-monitor][layout]") {
    constexpr std::array displays{ui::DisplayWorkArea{0, 0, 1920, 1040},
                                  ui::DisplayWorkArea{1920, -200, 2560, 1400}};
    CHECK(ui::choose_display_work_area(2100, 0, 1100, 700, displays.data(), displays.size()) == 1U);
    CHECK(ui::choose_display_work_area(100, 100, 1100, 700, displays.data(), displays.size()) == 0U);
}

TEST_CASE("high contrast palette preserves maximum text contrast", "[ui][accessibility][high-contrast]") {
    const auto palette = ui::accessibility_palette(true, true);
    CHECK(palette.background[0] == 0.0F);
    CHECK(palette.foreground[0] == 1.0F);
    CHECK(palette.accent != palette.warning);
}

TEST_CASE("product visual system keeps semantic colors and geometry explicit",
          "[ui][style]") {
    const auto standard = ui::product_visual_style(false);
    CHECK(standard.background != standard.surface);
    CHECK(standard.surface != standard.surface_raised);
    CHECK(standard.accent != standard.warning);
    CHECK(standard.success != standard.warning);
    CHECK(standard.window_rounding > 0.0F);
    CHECK(standard.child_rounding > 0.0F);
    CHECK(standard.frame_rounding > 0.0F);

    const auto high_contrast = ui::product_visual_style(true);
    CHECK(high_contrast.background[0] == 0.0F);
    CHECK(high_contrast.foreground[0] == 1.0F);
    CHECK(high_contrast.accent[1] == 1.0F);
    CHECK(high_contrast.window_rounding == 0.0F);
}

TEST_CASE("accessibility style switches to high contrast and reverses cleanly",
          "[ui][accessibility][high-contrast][interaction]") {
    const ScopedImGuiContext context;
    ui::apply_accessibility_style(false);
    CHECK(ImGui::GetStyle().WindowPadding.x == 16.0F);
    CHECK(ImGui::GetStyle().FramePadding.y == 6.0F);
    CHECK(ImGui::GetStyle().WindowRounding > 0.0F);
    const auto normal_background = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    const auto normal_button = ImGui::GetStyle().Colors[ImGuiCol_Button];

    bool high_contrast_enabled{};
    CHECK(ui::update_accessibility_style(high_contrast_enabled, true));
    CHECK(high_contrast_enabled);
    const auto palette = ui::accessibility_palette(true, true);
    const auto& high_contrast = ImGui::GetStyle().Colors;
    check_color(high_contrast[ImGuiCol_WindowBg], palette.background);
    check_color(high_contrast[ImGuiCol_Text], palette.foreground);
    check_color(high_contrast[ImGuiCol_CheckMark], palette.accent);
    CHECK(high_contrast[ImGuiCol_Border].x == 1.0F);
    CHECK(high_contrast[ImGuiCol_Separator].x == 1.0F);
    CHECK(high_contrast[ImGuiCol_ButtonHovered].y >
          high_contrast[ImGuiCol_Button].y);

    CHECK_FALSE(ui::update_accessibility_style(high_contrast_enabled, true));
    CHECK(ui::update_accessibility_style(high_contrast_enabled, false));
    CHECK_FALSE(high_contrast_enabled);
    const auto& restored = ImGui::GetStyle().Colors;
    CHECK(restored[ImGuiCol_WindowBg].x == normal_background.x);
    CHECK(restored[ImGuiCol_WindowBg].y == normal_background.y);
    CHECK(restored[ImGuiCol_WindowBg].z == normal_background.z);
    CHECK(restored[ImGuiCol_Button].x == normal_button.x);
    CHECK(restored[ImGuiCol_Button].y == normal_button.y);
    CHECK(restored[ImGuiCol_Button].z == normal_button.z);
}

TEST_CASE("default UI source stays inside the bundled Basic Latin font",
          "[ui][accessibility][font]") {
    const std::filesystem::path directory =
        std::filesystem::path{BLACKBOX_SOURCE_ROOT} / "src" / "ui";
    REQUIRE(std::filesystem::is_directory(directory));
    for (const auto& entry : std::filesystem::recursive_directory_iterator{directory}) {
        if (!entry.is_regular_file()) continue;
        const auto extension = entry.path().extension();
        if (extension != ".cpp" && extension != ".hpp") continue;
        std::ifstream input{entry.path(), std::ios::binary};
        REQUIRE(input.is_open());
        const std::string bytes{std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{}};
        INFO(entry.path().string());
        CHECK(std::ranges::all_of(bytes, [](const char value) {
            return static_cast<unsigned char>(value) <= 0x7FU;
        }));
    }
}
