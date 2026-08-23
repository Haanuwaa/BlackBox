#pragma once

#include "ui/dashboard.hpp"

namespace blackbox::ui::detail {

void render_product_settings(const DashboardState &state,
                             ProductUiState &product,
                             DashboardCommand &command);

} // namespace blackbox::ui::detail
