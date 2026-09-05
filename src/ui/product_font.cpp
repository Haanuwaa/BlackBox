#include "ui/product_font.hpp"
#include "ui/product_ui_model.hpp"
#include <imgui.h>
#include <array>
#include <filesystem>

namespace blackbox::ui {
void load_product_font(ImGuiIO& io, const float display_scale) {
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
