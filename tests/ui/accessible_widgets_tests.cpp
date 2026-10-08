#include "ui/accessible_widgets.hpp"
#include "ui/product_font.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>

namespace core = blackbox::core;
namespace accessible = blackbox::ui::accessible;

TEST_CASE("accessibility actions obey widget identity and disabled boundaries", "[ui][accessibility]") {
    struct Context {
        Context() { ImGui::CreateContext(); }
        ~Context() { ImGui::DestroyContext(); }
    } context;
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(640, 480);
    io.DeltaTime = 1.0F / 60.0F;
    unsigned char* pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    core::AccessibilityTree tree;
    tree.enable();
    bool checked{}, clicked{}, disabled{};
    char value[16] = "initial";
    const auto frame = [&] {
        ImGui::NewFrame();
        accessible::begin_frame(&tree);
        ImGui::Begin("Accessible test");
        ImGui::BeginDisabled(disabled);
        clicked = accessible::Button("Capture");
        accessible::Checkbox("Enabled", &checked);
        accessible::InputText("Note", value, sizeof(value));
        ImGui::EndDisabled();
        ImGui::End();
        accessible::end_frame();
        ImGui::Render();
    };
    frame(); frame();
    const auto id = [&](const char* name) {
        for (const auto& node : tree.snapshot()->nodes) if (node.name == name) return node.id;
        return std::uint32_t{};
    };
    const auto button = id("Capture"), checkbox = id("Enabled"), edit = id("Note");
    REQUIRE(button != 0U);
    REQUIRE(checkbox != 0U);
    REQUIRE(edit != 0U);
    CHECK_FALSE(tree.request({button, core::AccessibleAction::set_value, "wrong", {}}));
    REQUIRE(tree.request({button, core::AccessibleAction::focus, {}, {}}));
    REQUIRE(tree.request({button, core::AccessibleAction::invoke, {}, {}}));
    REQUIRE(tree.request({checkbox, core::AccessibleAction::toggle, {}, {}}));
    REQUIRE(tree.request({edit, core::AccessibleAction::set_value, "saved", {}}));
    frame();
    CHECK(clicked);
    CHECK(checked);
    CHECK(std::strcmp(value, "saved") == 0);
    REQUIRE(tree.request({edit, core::AccessibleAction::set_value, std::string(20, 'x'), {}}));
    frame();
    CHECK(std::strcmp(value, "saved") == 0); // Capacity checked by the real widget.
    REQUIRE(tree.request({button, core::AccessibleAction::invoke, {}, {}}));
    disabled = true; // State changes after the provider accepted the request.
    frame();
    CHECK_FALSE(clicked);
    CHECK_FALSE(tree.request({button, core::AccessibleAction::invoke, {}, {}}));
    tree.publish({}, false);
    CHECK_FALSE(tree.request({edit, core::AccessibleAction::set_value, "stale", {}}));
    CHECK(tree.take_requests().empty());
}

TEST_CASE("accessibility request storage stays bounded", "[ui][accessibility]") {
    core::AccessibilityTree tree;
    core::AccessibleNode button{};
    button.id = 1U;
    button.role = core::AccessibleRole::button;
    button.name = "Capture";
    tree.publish({button});
    for (int count = 0; count < 64; ++count)
        REQUIRE(tree.request({1U, core::AccessibleAction::invoke, {}, {}}));
    CHECK_FALSE(tree.request({1U, core::AccessibleAction::invoke, {}, {}}));
    CHECK(tree.take_requests().size() == 64U);
    CHECK_FALSE(tree.request({2U, core::AccessibleAction::invoke, {}, {}}));
}

TEST_CASE("product fonts defer installed CJK fallback until text needs it", "[ui][font]") {
#if defined(_WIN32)
    if (!std::filesystem::is_regular_file("C:/Windows/Fonts/msyh.ttc"))
        SKIP("Optional installed CJK font is unavailable on this host");
    struct Context {
        Context() { ImGui::CreateContext(); }
        ~Context() { ImGui::DestroyContext(); }
    } context;
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(640, 480);
    io.DeltaTime = 1.0F / 60.0F;
    blackbox::ui::load_product_font(io);
    unsigned char* pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const auto initial = io.Fonts->Sources.Size;
    ImGui::NewFrame();
    blackbox::ui::request_product_font_coverage("Ordinary preferences");
    ImGui::EndFrame();
    blackbox::ui::load_requested_product_fonts(io, 1.0F);
    CHECK(io.Fonts->Sources.Size == initial);
    ImGui::NewFrame();
    blackbox::ui::request_product_font_coverage("\xE6\xB5\x8B\xE8\xAF\x95");
    ImGui::EndFrame();
    blackbox::ui::load_requested_product_fonts(io, 1.0F);
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    CHECK(io.Fonts->Sources.Size == initial + 1);
    CHECK(io.Fonts->Fonts.front()->IsGlyphInFont(0x6D4BU));
    CHECK(io.Fonts->Fonts.front()->IsGlyphInFont(0x8BD5U));
#else
    SKIP("Installed Windows CJK font coverage is a Windows integration check");
#endif
}
