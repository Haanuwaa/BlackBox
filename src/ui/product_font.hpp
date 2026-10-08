#pragma once

#include <string_view>
struct ImGuiIO;

namespace blackbox::ui {
// App and raster qualification deliberately share the same font selection.
void request_product_font_coverage(std::string_view text);
void load_requested_product_fonts(ImGuiIO& io, float display_scale);
[[nodiscard]] bool product_font_coverage_probe_needed();
void load_product_font(ImGuiIO& io, float display_scale = 1.0F);
} // namespace blackbox::ui
