#pragma once

namespace blackbox::ui {

struct DashboardCommand;
struct IncidentViewerState;
struct ProductUiState;

namespace detail {

void render_incident_viewer(IncidentViewerState& state, DashboardCommand& command,
                            ProductUiState& product);

} // namespace detail
} // namespace blackbox::ui
