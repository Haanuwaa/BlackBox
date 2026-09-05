#pragma once

struct ImGuiIO;

namespace blackbox::ui {
// App and raster qualification deliberately share the same font selection.
void load_product_font(ImGuiIO& io, float display_scale = 1.0F);
} // namespace blackbox::ui
