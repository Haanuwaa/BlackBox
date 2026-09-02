#include "ui/dashboard_pressure.hpp"

#include <imgui.h>

namespace blackbox::ui::detail {
namespace {

[[nodiscard]] constexpr const char* status_text(const MetricDisplayStatus status) noexcept {
    switch (status) {
    case MetricDisplayStatus::available:
        return "Available";
    case MetricDisplayStatus::warming_up:
        return "Warming up";
    case MetricDisplayStatus::unsupported:
        return "Unsupported";
    case MetricDisplayStatus::inaccessible:
        return "Inaccessible";
    case MetricDisplayStatus::unavailable:
        return "Unavailable";
    }
    return "Unavailable";
}

void value_row(const char* label, const MetricDisplayStatus status, const double value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    if (status == MetricDisplayStatus::available) {
        ImGui::Text("%.2f%%", value * 100.0);
    } else {
        ImGui::TextDisabled("%s", status_text(status));
    }
}

[[nodiscard]] constexpr const char* thermal_text(const std::uint8_t state) noexcept {
    switch (state) {
    case 0U:
        return "Nominal";
    case 1U:
        return "Fair";
    case 2U:
        return "Serious";
    case 3U:
        return "Critical";
    default:
        return "Unknown";
    }
}

[[nodiscard]] constexpr const char* memory_pressure_text(const std::uint8_t state) noexcept {
    switch (state) {
    case 0U:
        return "Normal";
    case 1U:
        return "Warning";
    case 2U:
        return "Critical";
    default:
        return "Unknown";
    }
}

} // namespace

void render_pressure_rows(const DashboardState& state) {
    value_row("CPU stall pressure (some)", state.cpu_some_pressure_status, state.cpu_some_pressure);
    value_row("Memory stall pressure (some)", state.memory_some_pressure_status,
              state.memory_some_pressure);
    value_row("Memory stall pressure (full)", state.memory_full_pressure_status,
              state.memory_full_pressure);
    value_row("I/O stall pressure (some)", state.io_some_pressure_status, state.io_some_pressure);
    value_row("I/O stall pressure (full)", state.io_full_pressure_status, state.io_full_pressure);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("Thermal pressure state");
    ImGui::TableSetColumnIndex(1);
    if (state.thermal_pressure_status == MetricDisplayStatus::available) {
        ImGui::TextUnformatted(thermal_text(state.thermal_pressure_state));
    } else {
        ImGui::TextDisabled("%s", status_text(state.thermal_pressure_status));
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("Memory pressure state");
    ImGui::TableSetColumnIndex(1);
    if (state.memory_pressure_status == MetricDisplayStatus::available) {
        ImGui::TextUnformatted(memory_pressure_text(state.memory_pressure_state));
    } else {
        ImGui::TextDisabled("%s", status_text(state.memory_pressure_status));
    }
}

} // namespace blackbox::ui::detail
