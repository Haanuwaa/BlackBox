#include "ui/dashboard.hpp"
#include "ui/dashboard_incidents.hpp"
#include "ui/dashboard_pressure.hpp"
#include "ui/dashboard_settings.hpp"
#include "ui/product_ui_model.hpp"

#include "core/version.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <implot.h>
#include <string>

namespace blackbox::ui {
namespace {

void render_metric_unavailable(MetricDisplayStatus status);

[[nodiscard]] ImVec4 ui_color(const std::array<float, 4U>& value) noexcept {
    return ImVec4{value[0], value[1], value[2], value[3]};
}

[[nodiscard]] constexpr const char* connectivity_text(const std::uint8_t level) noexcept {
    switch (level) {
    case 0U:
        return "Disconnected";
    case 1U:
        return "Local access";
    case 2U:
        return "Internet access";
    case 3U:
        return "Constrained internet";
    default:
        return "Unknown";
    }
}

void render_product_header(const DashboardState& state, ProductUiState& product) {
    const auto visual = product_visual_style(state.accessibility_high_contrast);
    const auto available_width = ImGui::GetContentRegionAvail().x;
    const auto columns = navigation_column_count(available_width);
    const auto rows = (6U + columns - 1U) / columns;
    const auto header_height = 72.0F + static_cast<float>(rows) * 38.0F;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_color(visual.surface));
    if (ImGui::BeginChild("Product header", ImVec2{0.0F, header_height}, ImGuiChildFlags_Borders)) {
        ImGui::SetWindowFontScale(1.28F);
        ImGui::TextColored(ui_color(visual.accent), "BLACKBOX");
        ImGui::SetWindowFontScale(1.0F);
        ImGui::SameLine();
        ImGui::TextDisabled("Computer Flight Recorder");
        ImGui::TextDisabled("Private system history that helps explain slowdowns.  |  F1 shortcuts");

        constexpr const char* page_names[]{"Live",     "Incidents", "Explain",
                                           "Patterns", "Settings",  "Diagnostics"};
        constexpr ImGuiKey page_keys[]{ImGuiKey_1, ImGuiKey_2, ImGuiKey_3,
                                       ImGuiKey_4, ImGuiKey_5, ImGuiKey_6};
        static_assert(std::size(page_names) == std::size(page_keys));
        const auto spacing = ImGui::GetStyle().ItemSpacing.x;
        const auto button_width = std::max(
            72.0F, (ImGui::GetContentRegionAvail().x - spacing * static_cast<float>(columns - 1U)) /
                       static_cast<float>(columns));
        for (std::size_t index = 0U; index < std::size(page_names); ++index) {
            if (index != 0U && index % columns != 0U) ImGui::SameLine();
            const auto selected = product.page == static_cast<ProductPage>(index);
            ImGui::PushID(static_cast<int>(index));
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, ui_color(visual.accent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui_color(visual.accent_hovered));
                ImGui::PushStyleColor(ImGuiCol_Text, state.accessibility_high_contrast
                                                         ? ui_color(visual.background)
                                                         : ImVec4{1.0F, 1.0F, 1.0F, 1.0F});
            }
            if (ImGui::Button(page_names[index], ImVec2{button_width, 30.0F})) {
                product.page = static_cast<ProductPage>(index);
            }
            if (selected) ImGui::PopStyleColor(3);
            ImGui::PopID();
            const auto& io = ImGui::GetIO();
            const auto control_down = io.KeyCtrl || ImGui::IsKeyDown(ImGuiKey_LeftCtrl) ||
                                      ImGui::IsKeyDown(ImGuiKey_RightCtrl);
            // This assignment is intentionally idempotent while the chord is
            // held. It avoids depending on platform-specific first-frame
            // key-repeat timing.
            if (control_down && ImGui::IsKeyDown(page_keys[index])) {
                product.page = static_cast<ProductPage>(index);
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void render_metric_card(const char* id, const char* title, const MetricDisplayStatus status,
                        const double fraction, const char* overlay) {
    if (ImGui::BeginChild(id, ImVec2{0.0F, 86.0F}, ImGuiChildFlags_Borders)) {
        ImGui::TextDisabled("%s", title);
        ImGui::Spacing();
        if (status == MetricDisplayStatus::available) {
            ImGui::ProgressBar(static_cast<float>(std::clamp(fraction, 0.0, 1.0)),
                               ImVec2{-1.0F, 22.0F}, overlay);
        } else {
            render_metric_unavailable(status);
        }
    }
    ImGui::EndChild();
}

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
        return "Temporarily unavailable";
    }
    return "Unknown";
}

void render_metric_unavailable(const MetricDisplayStatus status) {
    ImGui::TextDisabled("%s", status_text(status));
}

void render_rate_row(const char* label, const MetricDisplayStatus status, const double value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    if (status == MetricDisplayStatus::available) {
        ImGui::Text("%.2f MiB/s", value);
    } else {
        render_metric_unavailable(status);
    }
}

} // namespace

bool set_timeline_cursor(ProductUiState& product, const double seconds_from_event,
                         const double incident_start_seconds,
                         const double incident_end_seconds) noexcept {
    if (!std::isfinite(seconds_from_event) || !std::isfinite(incident_start_seconds) ||
        !std::isfinite(incident_end_seconds) || incident_start_seconds > incident_end_seconds) {
        return false;
    }
    product.timeline_cursor_seconds =
        std::clamp(seconds_from_event, incident_start_seconds, incident_end_seconds);
    product.timeline_cursor_visible = true;
    return true;
}

void clear_timeline_cursor(ProductUiState& product) noexcept {
    product.timeline_cursor_visible = false;
    product.timeline_cursor_seconds = 0.0;
}

DashboardCommand render_dashboard(const DashboardState& state, IncidentViewerState& incident_viewer,
                                  ProductUiState& product) {
    DashboardCommand command{};

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("BlackBox Dashboard", nullptr, flags)) {
        render_product_header(state, product);
        ImGui::Spacing();

        const auto& io = ImGui::GetIO();
        const auto control_down = io.KeyCtrl || ImGui::IsKeyDown(ImGuiKey_LeftCtrl) ||
                                  ImGui::IsKeyDown(ImGuiKey_RightCtrl);
        if (!product.onboarding_open && ImGui::IsKeyPressed(ImGuiKey_F1)) {
            product.keyboard_help_open = true;
        }
        if (!product.onboarding_open && control_down && ImGui::IsKeyPressed(ImGuiKey_Enter) &&
            state.incident_capture_enabled) {
            command.action = DashboardAction::capture_incident;
        }
        if (!product.onboarding_open && control_down && ImGui::IsKeyPressed(ImGuiKey_R) &&
            product.page == ProductPage::incidents) {
            command.action = DashboardAction::refresh_incidents;
            command.incident_offset = 0U;
            command.incident_order = incident_viewer.order;
            command.search = incident_viewer.search.data();
        }

        if (product.keyboard_help_open) ImGui::OpenPopup("Keyboard shortcuts");
        ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing,
                                ImVec2{0.5F, 0.5F});
        if (ImGui::BeginPopupModal("Keyboard shortcuts", &product.keyboard_help_open,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped("Every primary workflow is available without a mouse.");
            if (ImGui::BeginTable("Keyboard shortcut list", 2,
                                  ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp)) {
                const auto shortcut = [](const char* keys, const char* action) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(keys);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(action);
                };
                shortcut("Tab / Shift+Tab", "Move between controls");
                shortcut("Enter / Space", "Activate the focused control");
                shortcut("Ctrl+1 ... Ctrl+6", "Open a primary page");
                shortcut("Ctrl+Enter", "Capture what just happened");
                shortcut("Ctrl+R", "Refresh the incident archive");
                shortcut("F1", "Show this shortcut guide");
                shortcut("Escape", "Close this guide");
                ImGui::EndTable();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::Button("Close")) {
                product.keyboard_help_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (product.onboarding_open) ImGui::OpenPopup("Welcome to BlackBox");
        const auto onboarding = onboarding_layout(viewport->WorkSize.x, viewport->WorkSize.y);
        ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, ImVec2{0.5F, 0.5F});
        ImGui::SetNextWindowSize(ImVec2{onboarding.width, onboarding.height}, ImGuiCond_Always);
        if (ImGui::BeginPopupModal("Welcome to BlackBox", nullptr,
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::SetWindowFontScale(onboarding.compact ? 1.08F : 1.22F);
            ImGui::TextUnformatted("Record first. Explain after the slowdown.");
            ImGui::SetWindowFontScale(1.0F);
            ImGui::TextDisabled("A private flight recorder for the computer you already use.");
            ImGui::Separator();

            const float footer_height = ImGui::GetFrameHeightWithSpacing() * 2.2F;
            if (ImGui::BeginChild("Onboarding steps", ImVec2{0.0F, -footer_height},
                                  ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
                ImGui::SeparatorText("1  Keep a rolling history");
                ImGui::TextWrapped("BlackBox quietly keeps a short resource history in memory. "
                                   "Recording continues when this window is minimized or hidden.");
                ImGui::SeparatorText("2  Capture the moment");
                ImGui::TextWrapped("When something feels wrong, select Capture what just "
                                   "happened or press Ctrl+Enter. A global shortcut can also be "
                                   "configured in Settings.");
                ImGui::SeparatorText("3  Review local evidence");
                ImGui::TextWrapped("Saved incidents stay on this computer unless you "
                                   "explicitly "
                                   "export them. BlackBox shows likely contributors and "
                                   "uncertainty; "
                                   "correlation is never presented as proof of cause.");

                ImGui::Spacing();
                ImGui::SeparatorText("Ready now");
                if (ImGui::BeginTable("Onboarding readiness", 2,
                                      ImGuiTableFlags_BordersInnerH |
                                          ImGuiTableFlags_SizingStretchProp)) {
                    const auto row = [](const char* label, const char* value) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextDisabled("%s", label);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextWrapped("%s", value);
                    };
                    row("Recorder", state.recorder_status.c_str());
                    row("Capture",
                        state.incident_capture_enabled ? "Ready" : "Temporarily unavailable");
                    row("Storage", state.storage_status.c_str());
                    row("Shortcut", state.hotkey_status.c_str());
                    row("Background", state.background_status.c_str());
                    row("Privacy", "Local unless you export");
                    ImGui::EndTable();
                }
                if (ImGui::CollapsingHeader("Warming up and unavailable")) {
                    ImGui::TextWrapped("Warming up means a counter needs an "
                                       "earlier observation. "
                                       "Unavailable means the operating system "
                                       "did not provide a "
                                       "reliable value. BlackBox keeps both "
                                       "states explicit instead of "
                                       "silently treating them as zero.");
                }
                ImGui::TextDisabled("Keyboard: Tab moves focus, Ctrl+Enter captures, Ctrl+1 "
                                    "through Ctrl+6 changes pages, and F1 shows all shortcuts.");
            }
            ImGui::EndChild();
            ImGui::Separator();
            if (ImGui::Button("Continue to live recorder", ImVec2{-FLT_MIN, 0.0F})) {
                product.onboarding_open = false;
                command.action = DashboardAction::complete_onboarding;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (product.page == ProductPage::live) {
            const auto visual = product_visual_style(state.accessibility_high_contrast);
            const bool recording = state.recorder_status == "Recording";
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_color(visual.surface));
            if (ImGui::BeginChild("Recorder summary", ImVec2{0.0F, 184.0F},
                                  ImGuiChildFlags_Borders)) {
                ImGui::TextDisabled("YOUR RECORDER");
                if (recording && state.incident_capture_enabled) {
                    ImGui::TextColored(ui_color(visual.success),
                                       "Ready - quietly recording recent activity");
                } else if (recording) {
                    ImGui::TextColored(ui_color(visual.warning), "Recording; incident capture is "
                                                                 "temporarily unavailable");
                } else {
                    ImGui::TextColored(ui_color(visual.warning), "Recorder is stopped");
                }
                ImGui::TextDisabled("Global shortcut: %s", state.hotkey_status.c_str());
                ImGui::TextDisabled("Background: %s", state.background_status.c_str());
                ImGui::TextDisabled("When the tray is active, closing this window hides it; "
                                    "recording continues.");
                if (!state.incident_capture_enabled) ImGui::BeginDisabled();
                ImGui::PushStyleColor(ImGuiCol_Button, ui_color(visual.accent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui_color(visual.accent_hovered));
                if (ImGui::Button("Capture what just happened   Ctrl+Enter",
                                  ImVec2{340.0F, 34.0F})) {
                    command.action = DashboardAction::capture_incident;
                }
                ImGui::PopStyleColor(2);
                if (!state.incident_capture_enabled) ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextUnformatted(state.incident_capture_status.c_str());
                ImGui::TextDisabled("Saved locally: %llu incident%s",
                                    static_cast<unsigned long long>(state.stored_incident_count),
                                    state.stored_incident_count == 1U ? "" : "s");
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            if (ImGui::CollapsingHeader("Technical status and capture details")) {
                if (ImGui::BeginTable("Status", 2,
                                      ImGuiTableFlags_BordersInnerH |
                                          ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthFixed, 180.0F);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.0F);

                    const auto row = [](const char* label, const char* value) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(label);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(value);
                    };

                    row("Recorder", state.recorder_status.data());
                    row("Platform", state.platform_name.data());
                    row("Telemetry Provider", state.provider_name.data());
                    row("Provider Status", state.provider_status.data());
                    row("Global hotkey", state.hotkey_status.data());
                    row("Background shell", state.background_status.data());
                    row("Incident archive", state.storage_status.data());
                    row("Accessibility", state.accessibility_high_contrast ? "Increased contrast"
                                                                           : "Standard contrast");
                    row("System animations", state.accessibility_animations_enabled
                                                 ? "Enabled"
                                                 : "Reduced by desktop preference");
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Display scale");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.0f%% | %u display%s | %ux%u px", state.display_scale * 100.0,
                                state.display_count, state.display_count == 1U ? "" : "s",
                                state.window_pixel_width, state.window_pixel_height);
                    row("Version", core::version.data());
                    ImGui::EndTable();
                }
                ImGui::TextWrapped("Unavailable metrics retain their reason (unsupported, "
                                   "inaccessible, or temporary). Cold-start values require a "
                                   "previous "
                                   "counter observation. Analysis is post-capture and "
                                   "correlation "
                                   "never establishes causation.");
                ImGui::Text("Window: %.0f s before / %.0f s after",
                            state.incident_pre_window_seconds, state.incident_post_window_seconds);
                if (state.incident_post_remaining_seconds > 0.0) {
                    ImGui::Text("Post-window remaining: %.1f s",
                                state.incident_post_remaining_seconds);
                }
                ImGui::Text("Writer handoff: %zu / %zu immutable incidents",
                            state.incident_queue_size, state.incident_queue_capacity);
                ImGui::Text(
                    "Automatic detection: %s | %llu triggers | %llu cooldown "
                    "suppressions | %llu rejected",
                    state.automatic_detection_enabled ? "enabled" : "disabled",
                    static_cast<unsigned long long>(state.automatic_detector_triggers),
                    static_cast<unsigned long long>(state.automatic_detector_cooldown_suppressions),
                    static_cast<unsigned long long>(state.automatic_capture_rejections));
                ImGui::TextWrapped(
                    "System-event symptom capture: %s | %llu event request%s",
                    state.automatic_event_capture_status.c_str(),
                    static_cast<unsigned long long>(state.automatic_event_capture_requests),
                    state.automatic_event_capture_requests == 1U ? "" : "s");
                ImGui::TextDisabled("Frame pacing capture: %s",
                                    state.automatic_frame_capture_status.c_str());
                ImGui::TextDisabled("Audio glitch capture: %s",
                                    state.automatic_audio_capture_status.c_str());
            }
        }

        if (product.page == ProductPage::incidents || product.page == ProductPage::detail ||
            product.page == ProductPage::patterns) {
            detail::render_incident_viewer(incident_viewer, command, product);
        }

        if (product.page == ProductPage::settings) {
            ImGui::Spacing();
            ImGui::SeparatorText("Recorder settings");
            ImGui::BeginChild("Recorder profiles", ImVec2{-1.0F, 118.0F}, ImGuiChildFlags_Borders);
            ImGui::TextUnformatted("Collection profile");
            ImGui::TextWrapped("Changing profile restarts the collector, "
                               "clears only rolling RAM "
                               "history, and never deletes saved incidents.");
            const auto profile_button = [&](const char* label, const std::uint64_t interval_ms,
                                            const std::uint64_t history_seconds) {
                const bool selected =
                    static_cast<std::uint64_t>(state.sample_interval_milliseconds) == interval_ms &&
                    static_cast<std::uint64_t>(state.history_duration_seconds) == history_seconds;
                if (selected) ImGui::BeginDisabled();
                if (ImGui::Button(label)) {
                    command.action = DashboardAction::apply_recorder_settings;
                    command.sample_interval_milliseconds = interval_ms;
                    command.history_duration_seconds = history_seconds;
                }
                if (selected) ImGui::EndDisabled();
            };
            profile_button("Conservative: 1 s / 5 min", 1'000U, 300U);
            ImGui::SameLine();
            profile_button("Balanced: 500 ms / 5 min", 500U, 300U);
            ImGui::SameLine();
            profile_button("Detailed: 250 ms / 2 min", 250U, 120U);
            ImGui::TextDisabled("%s", state.recorder_settings_status.data());
            ImGui::EndChild();
            ImGui::Spacing();
            detail::render_product_settings(state, product, command);
        }

        if (product.page == ProductPage::live) {
            ImGui::Spacing();
            ImGui::SeparatorText("What your computer is doing now");
            ImGui::TextDisabled("A quick local view of current resource activity. Missing values "
                                "stay marked instead of being shown as zero.");
            char cpu_overlay[32]{"Unavailable"};
            if (state.cpu_status == MetricDisplayStatus::available) {
                std::snprintf(cpu_overlay, sizeof(cpu_overlay), "%.1f%%", state.cpu_usage * 100.0);
            }
            char memory_overlay[64]{"Unavailable"};
            if (state.memory_status == MetricDisplayStatus::available) {
                const auto used_gib =
                    static_cast<double>(state.memory_used_bytes) / (1024.0 * 1024.0 * 1024.0);
                const auto total_gib =
                    static_cast<double>(state.memory_total_bytes) / (1024.0 * 1024.0 * 1024.0);
                std::snprintf(memory_overlay, sizeof(memory_overlay), "%.1f / %.1f GiB (%.1f%%)",
                              used_gib, total_gib, state.memory_usage * 100.0);
            }
            if (ImGui::BeginTable("Primary telemetry cards", 2,
                                  ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                render_metric_card("CPU card", "PROCESSOR", state.cpu_status, state.cpu_usage,
                                   cpu_overlay);
                ImGui::TableNextColumn();
                render_metric_card("Memory card", "MEMORY", state.memory_status,
                                   state.memory_usage, memory_overlay);
                ImGui::EndTable();
            }
            if (ImGui::BeginTable("Live throughput", 2,
                                  ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthFixed, 180.0F);
                ImGui::TableSetupColumn("Rate");
                render_rate_row("Storage read", state.disk_read_status,
                                state.disk_read_mib_per_second);
                render_rate_row("Storage write", state.disk_write_status,
                                state.disk_write_mib_per_second);
                render_rate_row("Download", state.network_receive_status,
                                state.network_receive_mib_per_second);
                render_rate_row("Upload", state.network_transmit_status,
                                state.network_transmit_mib_per_second);
                ImGui::EndTable();
            }
            if (ImGui::CollapsingHeader("More system details")) {
                ImGui::TextWrapped("Physical storage quality is separate from process I/O. "
                                   "Network "
                                   "quality is passive host-wide transport/connectivity "
                                   "evidence; no "
                                   "RTT probe or application payload is inspected.");
                if (ImGui::BeginTable("Live forensic quality", 2,
                                      ImGuiTableFlags_BordersInnerH |
                                          ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthFixed, 230.0F);
                    ImGui::TableSetupColumn("Observation");
                    const auto value_row = [](const char* label, const MetricDisplayStatus status,
                                              const char* format, const double value) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(label);
                        ImGui::TableSetColumnIndex(1);
                        if (status == MetricDisplayStatus::available)
                            ImGui::Text(format, value);
                        else
                            render_metric_unavailable(status);
                    };
                    value_row("Worst physical-disk service time", state.disk_latency_status,
                              "%.2f ms", state.disk_service_time_milliseconds);
                    value_row("Worst physical-disk queue", state.disk_queue_status, "%.2f requests",
                              state.disk_queue_depth);
                    value_row("Average physical-disk I/O in service",
                              state.disk_service_concurrency_status, "%.2f requests",
                              state.disk_service_concurrency);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Connectivity");
                    ImGui::TableSetColumnIndex(1);
                    if (state.network_connectivity_status == MetricDisplayStatus::available) {
                        ImGui::Text(
                            "%s | %llu active interface%s | %llu transition%s",
                            connectivity_text(state.network_connectivity_level),
                            static_cast<unsigned long long>(state.network_active_interfaces),
                            state.network_active_interfaces == 1U ? "" : "s",
                            static_cast<unsigned long long>(state.network_interface_changes),
                            state.network_interface_changes == 1U ? "" : "s");
                    } else {
                        render_metric_unavailable(state.network_connectivity_status);
                    }
                    value_row("Host-wide TCP retransmission",
                              state.network_transport_quality_status, "%.2f%%",
                              state.network_tcp_retransmit_percent);
                    value_row("Busiest GPU engine", state.gpu_status, "%.1f%%",
                              state.gpu_usage * 100.0);
                    value_row("Dedicated GPU memory", state.gpu_memory_status, "%.1f MiB",
                              state.gpu_dedicated_memory_mib);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("GPU inventory / renderer");
                    ImGui::TableSetColumnIndex(1);
                    if (state.gpu_inventory_status == MetricDisplayStatus::available) {
                        ImGui::Text("%u device%s (%u integrated, %u discrete, %u type "
                                    "unknown) | "
                                    "public render device %s | %s backend %s",
                                    state.gpu_device_count, state.gpu_device_count == 1U ? "" : "s",
                                    state.gpu_integrated_device_count,
                                    state.gpu_discrete_device_count, state.gpu_unknown_device_count,
                                    state.gpu_render_device_available ? "available" : "unavailable",
                                    state.renderer_active ? "active" : "inactive",
                                    state.renderer_backend.c_str());
                    } else {
                        ImGui::Text("Inventory unavailable | %s backend %s",
                                    state.renderer_active ? "active" : "inactive",
                                    state.renderer_backend.c_str());
                    }
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("BlackBox renderer health");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%llu frames | %llu hitches | %.2f ms p95 | %.2f ms max | "
                                "%llu present failures",
                                static_cast<unsigned long long>(state.renderer_frames),
                                static_cast<unsigned long long>(state.renderer_hitches),
                                state.renderer_frame_p95_milliseconds,
                                state.renderer_frame_maximum_milliseconds,
                                static_cast<unsigned long long>(state.renderer_present_failures));
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Delayed app diagnostics");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s | %llu metric / %llu diagnostic payloads | "
                                       "%.1f s app CPU | %.1f s app GPU | %llu hangs (%.1f s)",
                                       state.app_performance_report_status.c_str(),
                                       static_cast<unsigned long long>(state.app_metric_payloads),
                                       static_cast<unsigned long long>(
                                           state.app_diagnostic_payloads),
                                       state.app_cumulative_cpu_seconds,
                                       state.app_cumulative_gpu_seconds,
                                       static_cast<unsigned long long>(
                                           state.app_hang_diagnostics),
                                       state.app_hang_duration_seconds);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("GPU capability boundary");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", state.gpu_usage_support.c_str());
                    value_row("DPC processor time", state.dpc_status, "%.2f%%",
                              state.dpc_usage * 100.0);
                    value_row("Interrupt processor time", state.dpc_status, "%.2f%%",
                              state.interrupt_usage * 100.0);
                    value_row("Average CPU frequency", state.cpu_frequency_status, "%.0f MHz",
                              state.cpu_current_mhz);
                    value_row("CPU thermal ceiling", state.cpu_thermal_limit_status, "%.1f%%",
                              state.cpu_thermal_limit_fraction * 100.0);
                    detail::render_pressure_rows(state);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Foreground application");
                    ImGui::TableSetColumnIndex(1);
                    if (state.foreground_status == MetricDisplayStatus::available) {
                        if (state.foreground_application_opaque) {
                            ImGui::Text("Private application %06llX | process/GPU correlation "
                                        "unavailable",
                                        static_cast<unsigned long long>(
                                            state.foreground_application_token & 0xFFFFFFU));
                        } else {
                            ImGui::Text("PID %u | foreground GPU %.1f%%", state.foreground_pid,
                                        state.foreground_gpu_usage * 100.0);
                        }
                    } else {
                        render_metric_unavailable(state.foreground_status);
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("System-event recorder: %s | %zu / %zu "
                                    "events | %llu recorded | "
                                    "%llu dropped/overwritten",
                                    state.event_collector_running ? "running" : "disabled/stopped",
                                    state.system_event_ring_size, state.system_event_ring_capacity,
                                    static_cast<unsigned long long>(state.system_events_recorded),
                                    static_cast<unsigned long long>(state.system_events_dropped));
                ImGui::TextDisabled(
                    "Process lifecycle: %llu provider observations | "
                    "%llu opt-in events recorded",
                    static_cast<unsigned long long>(state.process_lifecycle_observations),
                    static_cast<unsigned long long>(state.process_lifecycle_events_recorded));
            }
        }

        if (product.page == ProductPage::live) {
            ImGui::Spacing();
            if (ImGui::CollapsingHeader("Recent activity", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextDisabled("The newest sample is at 0 seconds. Hover a graph to inspect "
                                    "values; gaps mean the metric was unavailable.");
                if (state.history_size == 0U) {
                    ImGui::TextDisabled("Waiting for recorder samples");
                } else if (ImPlot::BeginPlot("##system-history", ImVec2{-1.0F, 220.0F})) {
                    ImPlot::SetupAxes("Seconds from now", "Utilization (%)");
                    const auto oldest_sample = std::min(-1.0, state.history_oldest_seconds);
                    ImPlot::SetupAxisLimits(ImAxis_X1, oldest_sample, 0.0, ImGuiCond_Always);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 100.0, ImGuiCond_Always);
                    ImPlotSpec cpu_style{};
                    cpu_style.LineColor = ImVec4{0.20F, 0.80F, 1.00F, 1.00F};
                    cpu_style.LineWeight = 2.0F;
                    cpu_style.Marker = ImPlotMarker_Circle;
                    cpu_style.MarkerSize = 3.0F;
                    cpu_style.MarkerFillColor = cpu_style.LineColor;
                    cpu_style.MarkerLineColor = cpu_style.LineColor;
                    ImPlot::PlotLine("CPU utilization##cpu-history-series",
                                     state.cpu_history_x.data(), state.cpu_history.data(),
                                     static_cast<int>(state.cpu_history_points), cpu_style);
                    ImPlotSpec memory_style{};
                    memory_style.LineColor = ImVec4{1.00F, 0.65F, 0.20F, 1.00F};
                    memory_style.LineWeight = 2.0F;
                    memory_style.Marker = ImPlotMarker_Circle;
                    memory_style.MarkerSize = 3.0F;
                    memory_style.MarkerFillColor = memory_style.LineColor;
                    memory_style.MarkerLineColor = memory_style.LineColor;
                    ImPlot::PlotLine("Memory utilization##memory-history-series",
                                     state.memory_history_x.data(), state.memory_history.data(),
                                     static_cast<int>(state.memory_history_points), memory_style);
                    ImPlot::EndPlot();
                }

                const auto oldest_sample = std::min(-1.0, state.history_oldest_seconds);
                if (state.disk_read_history_points != 0U || state.disk_write_history_points != 0U) {
                    if (ImPlot::BeginPlot("Storage activity", ImVec2{-1.0F, 180.0F})) {
                        ImPlot::SetupAxes("Seconds from now", "MiB/s");
                        ImPlot::SetupAxisLimits(ImAxis_X1, oldest_sample, 0.0, ImGuiCond_Always);
                        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0,
                                                state.disk_history_max_mib_per_second * 1.1,
                                                ImGuiCond_Always);
                        ImPlot::PlotLine("Read", state.disk_read_history_x.data(),
                                         state.disk_read_history.data(),
                                         static_cast<int>(state.disk_read_history_points));
                        ImPlot::PlotLine("Write", state.disk_write_history_x.data(),
                                         state.disk_write_history.data(),
                                         static_cast<int>(state.disk_write_history_points));
                        ImPlot::EndPlot();
                    }
                } else {
                    ImGui::TextDisabled("Disk throughput history is warming up");
                }

                if (state.network_receive_history_points != 0U ||
                    state.network_transmit_history_points != 0U) {
                    if (ImPlot::BeginPlot("Network activity", ImVec2{-1.0F, 180.0F})) {
                        ImPlot::SetupAxes("Seconds from now", "MiB/s");
                        ImPlot::SetupAxisLimits(ImAxis_X1, oldest_sample, 0.0, ImGuiCond_Always);
                        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0,
                                                state.network_history_max_mib_per_second * 1.1,
                                                ImGuiCond_Always);
                        ImPlot::PlotLine("Receive", state.network_receive_history_x.data(),
                                         state.network_receive_history.data(),
                                         static_cast<int>(state.network_receive_history_points));
                        ImPlot::PlotLine("Transmit", state.network_transmit_history_x.data(),
                                         state.network_transmit_history.data(),
                                         static_cast<int>(state.network_transmit_history_points));
                        ImPlot::EndPlot();
                    }
                } else {
                    ImGui::TextDisabled("Network throughput history is warming up");
                }
            }

            ImGui::Spacing();
            if (ImGui::CollapsingHeader("Apps using resources (highest CPU first)")) {
                if (state.process_count == 0U) {
                    ImGui::TextDisabled("Process telemetry is warming up");
                } else if (ImGui::BeginTable("Active processes", 6,
                                             ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                                 ImGuiTableFlags_ScrollY |
                                                 ImGuiTableFlags_SizingStretchProp,
                                             ImVec2{-1.0F, 260.0F})) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Process");
                    ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 70.0F);
                    ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthFixed, 80.0F);
                    ImGui::TableSetupColumn("Working set", ImGuiTableColumnFlags_WidthFixed,
                                            100.0F);
                    ImGui::TableSetupColumn("Read", ImGuiTableColumnFlags_WidthFixed, 90.0F);
                    ImGui::TableSetupColumn("Write", ImGuiTableColumnFlags_WidthFixed, 90.0F);
                    ImGui::TableHeadersRow();
                    for (std::size_t index = 0U; index < state.process_count; ++index) {
                        const auto& process = state.processes[index];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(process.name.c_str());
                        if (!process.executable_path.empty() && ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", process.executable_path.c_str());
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%u", process.pid);
                        ImGui::TableSetColumnIndex(2);
                        process.cpu_status == MetricDisplayStatus::available
                            ? ImGui::Text("%.1f%%", process.cpu_percent)
                            : render_metric_unavailable(process.cpu_status);
                        ImGui::TableSetColumnIndex(3);
                        process.memory_status == MetricDisplayStatus::available
                            ? ImGui::Text("%.1f MiB", process.working_set_mib)
                            : render_metric_unavailable(process.memory_status);
                        ImGui::TableSetColumnIndex(4);
                        process.disk_read_status == MetricDisplayStatus::available
                            ? ImGui::Text("%.2f MiB/s", process.disk_read_mib_per_second)
                            : render_metric_unavailable(process.disk_read_status);
                        ImGui::TableSetColumnIndex(5);
                        process.disk_write_status == MetricDisplayStatus::available
                            ? ImGui::Text("%.2f MiB/s", process.disk_write_mib_per_second)
                            : render_metric_unavailable(process.disk_write_status);
                    }
                    ImGui::EndTable();
                }
            }
        }

        if (product.page == ProductPage::diagnostics) {
            ImGui::Spacing();
            ImGui::SeparatorText("Support and crash recovery");
            ImGui::TextWrapped("%s", state.crash_diagnostics_status.data());
            ImGui::Text("Completed local crash evidence: %llu | handler: %s",
                        static_cast<unsigned long long>(state.previous_crash_evidence),
                        state.crash_diagnostics_armed ? "armed" : "not armed");
            ImGui::TextWrapped("%s", state.support_bundle_status.data());
            ImGui::TextDisabled("Bundles stay local and exclude incidents, "
                                "process rows, settings, "
                                "hotkeys, usernames, and absolute paths.");
            ImGui::InputText("New support bundle directory", product.support_bundle_path.data(),
                             product.support_bundle_path.size());
            if (!state.latest_crash_evidence_available) {
                product.include_latest_crash_evidence = false;
                product.crash_evidence_consent_confirmed = false;
                ImGui::BeginDisabled();
            }
            ImGui::Checkbox("Include latest raw crash evidence",
                            &product.include_latest_crash_evidence);
            if (!state.latest_crash_evidence_available) ImGui::EndDisabled();
            if (product.include_latest_crash_evidence) {
                ImGui::TextColored(ImVec4{1.0F, 0.75F, 0.25F, 1.0F},
                                   "Depending on platform, crash evidence can contain "
                                   "memory, addresses, and module paths.");
                ImGui::Checkbox("I consent to place the latest raw evidence in "
                                "this local bundle",
                                &product.crash_evidence_consent_confirmed);
            } else {
                product.crash_evidence_consent_confirmed = false;
            }
            const bool support_disabled =
                state.support_bundle_busy || (product.include_latest_crash_evidence &&
                                              !product.crash_evidence_consent_confirmed);
            if (support_disabled) ImGui::BeginDisabled();
            if (ImGui::Button("Create local support bundle")) {
                command.action = DashboardAction::create_support_bundle;
                command.support_bundle_path = product.support_bundle_path.data();
                command.include_latest_crash_evidence = product.include_latest_crash_evidence;
                command.crash_evidence_disclosure_confirmed =
                    product.crash_evidence_consent_confirmed;
                product.crash_evidence_consent_confirmed = false;
            }
            if (support_disabled) ImGui::EndDisabled();

            ImGui::Spacing();
            ImGui::SeparatorText("Recorder diagnostics");
            if (ImGui::BeginTable("Recorder statistics", 2,
                                  ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Statistic", ImGuiTableColumnFlags_WidthFixed, 180.0F);
                ImGui::TableSetupColumn("Value");
                const auto count_row = [](const char* label, const std::uint64_t value) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(label);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%llu", static_cast<unsigned long long>(value));
                };
                const auto text_row = [](const char* label, const char* format,
                                         const double value) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(label);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text(format, value);
                };

                count_row("Collections", state.collection_count);
                count_row("CPU graph points", static_cast<std::uint64_t>(state.cpu_history_points));
                count_row("Memory graph points",
                          static_cast<std::uint64_t>(state.memory_history_points));
                count_row("Disk read graph points",
                          static_cast<std::uint64_t>(state.disk_read_history_points));
                count_row("Network RX graph points",
                          static_cast<std::uint64_t>(state.network_receive_history_points));
                count_row("Active processes",
                          static_cast<std::uint64_t>(state.active_process_count));
                count_row("Process metadata",
                          static_cast<std::uint64_t>(state.process_metadata_count));
                count_row("Metadata evictions", state.process_metadata_evictions);
                count_row("Inaccessible observations", state.process_inaccessible);
                count_row("Sampling exits", state.process_exits_during_sampling);
                count_row("Truncated process rows", state.process_samples_truncated);
                count_row("Incident captures started", state.incident_captures_started);
                count_row("Overlapping triggers merged", state.incident_requests_merged);
                count_row("One-observation quality triggers",
                          state.automatic_detector_single_observation_triggers);
                count_row("Incidents completed", state.incidents_completed);
                count_row("Incident queue rejections", state.incident_queue_rejections);
                count_row("Incident snapshot failures", state.incident_snapshot_failures);
                count_row("Incident captures cancelled", state.incident_captures_cancelled);
                count_row("Stored incidents", state.stored_incident_count);
                count_row("Writer attempts", state.storage_write_attempts);
                count_row("Writer retry attempts", state.storage_retry_attempts);
                count_row("Writer retries exhausted", state.storage_retry_exhausted);
                count_row("Writer successes", state.storage_write_successes);
                count_row("Writer failures", state.storage_write_failures);
                count_row("Writer cancellations", state.storage_write_cancellations);
                count_row("Viewer queued reads", state.viewer_read_queue_depth);
                count_row("Viewer queued mutations", state.viewer_mutation_queue_depth);
                count_row("Viewer reads coalesced", state.viewer_reads_coalesced);
                count_row("Viewer reads cancelled", state.viewer_reads_cancelled);
                count_row("Viewer mutations rejected", state.viewer_mutations_rejected);
                count_row("Viewer mutations completed", state.viewer_mutations_completed);
                count_row("Viewer mutations failed", state.viewer_mutations_failed);
                count_row("Partial samples", state.partial_samples);
                count_row("Failed samples", state.failed_samples);
                count_row("Dropped ticks", state.dropped_samples);
                count_row("Late starts", state.late_samples);
                count_row("Deadline misses", state.deadline_misses);
                count_row("Resume events", state.resume_events);
                count_row("Resume skipped ticks", state.resume_skipped_samples);
                count_row("Provider recoveries", state.provider_recoveries);
                count_row("Consecutive provider failures", state.consecutive_provider_failures);
                count_row("Collector worker failures", state.collector_worker_failures);
                if (state.resume_events != 0U) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Last resume gap");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.3f s", state.last_resume_gap_seconds);
                }
                count_row("Ring overwrites", state.ring_overwrites);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted("Ring utilization");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%zu / %zu (%.1f%%)", state.ring_size, state.ring_capacity,
                            state.ring_utilization * 100.0);
                text_row("Sample interval", "%.0f ms", state.sample_interval_milliseconds);
                text_row("History duration", "%.0f s", state.history_duration_seconds);
                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Collection timing");
            if (state.timing_samples == 0U) {
                ImGui::TextDisabled("No samples collected");
            } else if (ImGui::BeginTable("Collection timing", 2,
                                         ImGuiTableFlags_BordersInnerH |
                                             ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Statistic", ImGuiTableColumnFlags_WidthFixed, 180.0F);
                ImGui::TableSetupColumn("Value");

                const auto timing_row = [](const char* label, const double microseconds) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(label);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.2f us", microseconds);
                };

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted("Samples");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%llu", static_cast<unsigned long long>(state.timing_samples));
                timing_row("Average", state.timing_average_microseconds);
                timing_row("P50", state.timing_p50_microseconds);
                timing_row("P95", state.timing_p95_microseconds);
                timing_row("P99", state.timing_p99_microseconds);
                timing_row("Maximum", state.timing_maximum_microseconds);
                timing_row("Jitter average", state.jitter_average_microseconds);
                timing_row("Jitter P50", state.jitter_p50_microseconds);
                timing_row("Jitter P95", state.jitter_p95_microseconds);
                timing_row("Jitter P99", state.jitter_p99_microseconds);
                timing_row("Jitter maximum", state.jitter_maximum_microseconds);
                timing_row("Snapshot average", state.incident_snapshot_average_microseconds);
                timing_row("Snapshot P95", state.incident_snapshot_p95_microseconds);
                timing_row("Snapshot P99", state.incident_snapshot_p99_microseconds);
                timing_row("Snapshot maximum", state.incident_snapshot_maximum_microseconds);
                timing_row("Writer average", state.storage_write_average_microseconds);
                timing_row("Writer P95", state.storage_write_p95_microseconds);
                timing_row("Writer P99", state.storage_write_p99_microseconds);
                timing_row("Writer maximum", state.storage_write_maximum_microseconds);
                ImGui::EndTable();
            }
        }
    }
    ImGui::End();
    return command;
}

} // namespace blackbox::ui
