#pragma once

#include "ui/dashboard.hpp"

namespace blackbox::ui::detail {

[[nodiscard]] std::array<char, product_path_capacity + 1U>&
path_buffer(ProductUiState& product, PathField field) noexcept;
void render_path_input(const char* label,
                       std::array<char, product_path_capacity + 1U>& path,
                       PathField field, const ProductUiState& product,
                       DashboardCommand& command);

void render_product_settings(const DashboardState &state,
                             ProductUiState &product,
                             DashboardCommand &command);

} // namespace blackbox::ui::detail
