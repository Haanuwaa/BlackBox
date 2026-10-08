#include "ui/product_font.hpp"
#include "ui/product_ui_model.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <array>
#include <filesystem>

namespace blackbox::ui {
namespace {
thread_local bool fallback_requested{};
thread_local bool fallback_attempted{};
}
bool product_font_coverage_probe_needed() { return !fallback_attempted && !fallback_requested; }
void request_product_font_coverage(std::string_view text) {
    if (!product_font_coverage_probe_needed() || !ImGui::GetCurrentContext()) return;
    auto* font = ImGui::GetFont();
    const char* cursor = text.data();
    const char* end = cursor + text.size();
    while (cursor < end) {
        unsigned int point{};
        const int length = ImTextCharFromUtf8(&point, cursor, end);
        if (length <= 0) break;
        cursor += length;
        if (point >= 0x2E80U && point <= 0xFFFFU && !font->IsGlyphInFont(static_cast<ImWchar>(point))) {
            fallback_requested = true;
            break;
        }
    }
}
void load_requested_product_fonts(ImGuiIO& io, float display_scale) {
    if (!fallback_requested || fallback_attempted) return;
    fallback_attempted = true;
    fallback_requested = false;
#if defined(_WIN32)
    constexpr std::array candidates{"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/meiryo.ttc", "C:/Windows/Fonts/malgun.ttf"};
#elif defined(__APPLE__)
    constexpr std::array candidates{"/System/Library/Fonts/PingFang.ttc"};
#else
    constexpr std::array candidates{"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"};
#endif
    for (const auto* candidate : candidates) {
        std::error_code error{};
        const auto bytes = std::filesystem::file_size(candidate, error);
        if (error || bytes > 32U * 1024U * 1024U) continue;
        ImFontConfig config{};
        config.MergeMode = true;
        // One installed fallback, loaded only after visible text needs it. The
        // dynamic atlas rasterizes requested glyphs, not the entire CJK range.
        if (io.Fonts->AddFontFromFileTTF(candidate, 17.0F * normalize_display_scale(display_scale), &config)) break;
    }
}

void load_product_font(ImGuiIO& io, const float display_scale) {
    fallback_requested = false;
    fallback_attempted = false;
#if defined(_WIN32)
    constexpr std::array candidates{"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"};
#elif defined(__APPLE__)
    constexpr std::array candidates{"/System/Library/Fonts/SFNS.ttf",
                                    "/System/Library/Fonts/Helvetica.ttc"};
#else
    constexpr std::array candidates{
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"};
#endif
    for (const auto* candidate : candidates) {
        std::error_code error{};
        if (!std::filesystem::is_regular_file(candidate, error) || error) continue;
        if (io.Fonts->AddFontFromFileTTF(
                candidate, 17.0F * normalize_display_scale(display_scale)) != nullptr) {
            return;
        }
    }
    ImFontConfig fallback{};
    fallback.SizePixels = 13.0F * normalize_display_scale(display_scale);
    static_cast<void>(io.Fonts->AddFontDefault(&fallback));
}

} // namespace blackbox::ui
