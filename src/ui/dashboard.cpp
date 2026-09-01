#include "ui/dashboard.hpp"
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
        ImGui::TextDisabled("Local evidence. Honest uncertainty. Recording stays independent.");

        constexpr const char* page_names[]{"Live",     "Incidents", "Detail",
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

void render_disabled_wrapped(const char* const text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
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

void render_compact_decimal(const double value, const char* ordinary_format) {
    if (std::abs(value) >= 100'000.0) {
        ImGui::Text("%.3g", value);
    } else {
        ImGui::Text(ordinary_format, value);
    }
}

void render_series(const char* label, const IncidentPlotSeries& series) {
    if (!series.values.empty()) {
        ImPlot::PlotLine(label, series.seconds_from_event.data(), series.values.data(),
                         static_cast<int>(series.values.size()));
    }
}

void render_event_marker() {
    constexpr double event = 0.0;
    ImPlotSpec style{};
    style.LineColor = ImVec4{1.0F, 0.35F, 0.20F, 1.0F};
    style.LineWeight = 2.0F;
    ImPlot::PlotInfLines("Incident marker", &event, 1, style);
    ImPlot::TagX(event, style.LineColor, "INCIDENT");
}

void render_timeline_references(ProductUiState& product, const double incident_start_seconds,
                                const double incident_end_seconds) {
    if (ImPlot::IsPlotHovered()) {
        static_cast<void>(set_timeline_cursor(product,
                                              ImPlot::GetPlotMousePos(ImAxis_X1, ImAxis_Y1).x,
                                              incident_start_seconds, incident_end_seconds));
    }
    render_event_marker();
    if (!product.timeline_cursor_visible) return;
    ImPlotSpec style{};
    style.LineColor = ImVec4{0.20F, 0.80F, 1.0F, 0.95F};
    style.LineWeight = 1.5F;
    ImPlot::PlotInfLines("Synchronized cursor", &product.timeline_cursor_seconds, 1, style);
}

void render_missing_summary(const char* label, const IncidentPlotSeries& series) {
    if (series.values.empty()) {
        ImGui::TextDisabled("%s unavailable (unsupported %zu, inaccessible %zu, temporary %zu)",
                            label, series.availability.by_status[1],
                            series.availability.by_status[2], series.availability.by_status[3]);
    }
}

[[nodiscard]] constexpr const char*
automatic_resource_text(const core::AutomaticIncidentResource resource) noexcept {
    switch (resource) {
    case core::AutomaticIncidentResource::none:
        return "unknown resource";
    case core::AutomaticIncidentResource::cpu:
        return "CPU";
    case core::AutomaticIncidentResource::memory:
        return "memory";
    case core::AutomaticIncidentResource::disk:
        return "storage";
    case core::AutomaticIncidentResource::network:
        return "network";
    }
    return "unknown resource";
}

[[nodiscard]] constexpr const char*
automatic_signal_text(const core::AutomaticIncidentSignal signal) noexcept {
    switch (signal) {
    case core::AutomaticIncidentSignal::throughput_or_utilization:
        return "throughput/utilization";
    case core::AutomaticIncidentSignal::disk_latency:
        return "physical-disk latency";
    case core::AutomaticIncidentSignal::disk_queue_depth:
        return "physical-disk queue depth";
    case core::AutomaticIncidentSignal::network_connectivity:
        return "connectivity loss";
    case core::AutomaticIncidentSignal::network_interface_transition:
        return "interface/connectivity transition";
    case core::AutomaticIncidentSignal::tcp_retransmission:
        return "host-wide TCP retransmission";
    case core::AutomaticIncidentSignal::tcp_connection_failure:
        return "host-wide failed TCP connections";
    case core::AutomaticIncidentSignal::tcp_connection_reset:
        return "host-wide reset TCP connections";
    case core::AutomaticIncidentSignal::application_crash:
        return "application crash event";
    case core::AutomaticIncidentSignal::application_hang:
        return "application hang event";
    case core::AutomaticIncidentSignal::display_driver_recovery:
        return "display recovery event";
    case core::AutomaticIncidentSignal::storage_io_retry:
        return "storage I/O retry event";
    }
    return "unknown signal";
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

void request_page(DashboardCommand& command, const IncidentViewerState& state,
                  const std::size_t offset) {
    command.action = DashboardAction::refresh_incidents;
    command.incident_offset = offset;
    command.incident_order = state.order;
    command.search = state.search.data();
}

void render_incident_viewer(IncidentViewerState& state, DashboardCommand& command,
                            ProductUiState& product) {
    synchronize_incident_editor(state);
    ImGui::Spacing();
    ImGui::SeparatorText("Incident archive viewer");
    if (!state.content) {
        ImGui::TextDisabled("Viewer unavailable");
        return;
    }
    const auto& content = *state.content;
    if (product.page == ProductPage::incidents) {
        ImGui::BeginChild("Incident archive surface", ImVec2{-1.0F, 282.0F},
                          ImGuiChildFlags_Borders);
        ImGui::SetNextItemWidth(260.0F);
        ImGui::InputTextWithHint("##incident-search", "Search labels and notes",
                                 state.search.data(), state.search.size());
        ImGui::SameLine();
        if (ImGui::Button("Search / refresh")) request_page(command, state, 0U);
        ImGui::SameLine();
        constexpr const char* order_names[]{"Newest",   "Oldest",    "Longest",
                                            "Shortest", "Label A-Z", "Label Z-A"};
        auto order_index = static_cast<int>(state.order);
        ImGui::SetNextItemWidth(130.0F);
        if (ImGui::Combo("##incident-order", &order_index, order_names,
                         static_cast<int>(std::size(order_names)))) {
            state.order = static_cast<IncidentListOrder>(order_index);
            request_page(command, state, 0U);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s | query %.2f ms | view build %.2f ms | analysis %.2f ms",
                            content.status.c_str(), content.last_query_milliseconds,
                            content.last_build_milliseconds, content.last_analysis_milliseconds);

        const auto presentation = incident_archive_presentation(
            content.state, content.total_matching, state.search.front() != '\0');
        if (presentation != IncidentArchivePresentation::results) {
            ImGui::Spacing();
            switch (presentation) {
            case IncidentArchivePresentation::loading:
                ImGui::TextUnformatted("Loading saved incidents...");
                ImGui::TextDisabled("The recorder continues while the archive "
                                    "view is prepared.");
                break;
            case IncidentArchivePresentation::empty:
                ImGui::TextUnformatted("No incidents saved yet");
                ImGui::TextWrapped("BlackBox is still recording its bounded history. After a "
                                   "slowdown, use the configured hotkey or the Live page "
                                   "capture "
                                   "button to save what just happened.");
                if (ImGui::Button("Go to Live capture")) product.page = ProductPage::live;
                break;
            case IncidentArchivePresentation::no_matches:
                ImGui::TextUnformatted("No incidents match this search");
                ImGui::TextWrapped("Saved evidence has not been removed. Clear the "
                                   "search to return to the complete local archive.");
                if (ImGui::Button("Clear search")) {
                    state.search.fill('\0');
                    request_page(command, state, 0U);
                }
                break;
            case IncidentArchivePresentation::unavailable:
                ImGui::TextUnformatted("Incident archive unavailable");
                ImGui::TextWrapped("Recording can continue in memory, but saved "
                                   "incidents cannot be listed right now. Check "
                                   "Settings for archive health and guided recovery.");
                ImGui::TextDisabled("%s", content.status.c_str());
                if (ImGui::Button("Open archive recovery")) {
                    product.page = ProductPage::settings;
                }
                ImGui::SameLine();
                if (ImGui::Button("Retry archive view")) request_page(command, state, 0U);
                break;
            case IncidentArchivePresentation::results:
                break;
            }
            ImGui::EndChild();
            return;
        }

        if (ImGui::BeginTable("Archived incidents", 5,
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                              ImVec2{-1.0F, 190.0F})) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Captured (UTC)", ImGuiTableColumnFlags_WidthFixed, 190.0F);
            ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 80.0F);
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0F);
            ImGui::TableSetupColumn("Note");
            ImGui::TableSetupColumn("Rows", ImGuiTableColumnFlags_WidthFixed, 90.0F);
            ImGui::TableHeadersRow();
            for (const auto& row : content.incidents) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const auto selected = content.detail && content.detail->id == row.id;
                const auto identity = row.created_utc + "##incident-" + std::to_string(row.id);
                if (ImGui::Selectable(identity.c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    command.action = DashboardAction::select_incident;
                    command.incident_id = row.id;
                    product.page = ProductPage::detail;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.2f s", row.duration_seconds);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(row.label.empty() ? "(unlabeled)" : row.label.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(row.note_preview.c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%zu / %zu", row.system_sample_count, row.process_sample_count);
            }
            ImGui::EndTable();
        }
        const auto page_size = incident_list_page_size;
        if (content.offset != 0U && ImGui::Button("Previous page")) {
            request_page(command, state,
                         content.offset > page_size ? content.offset - page_size : 0U);
        }
        if (content.offset != 0U) ImGui::SameLine();
        if (content.offset + content.incidents.size() < content.total_matching &&
            ImGui::Button("Next page")) {
            request_page(command, state, content.offset + page_size);
        }

        ImGui::TextDisabled("Saved incidents are immutable evidence; labels, "
                            "notes, and classifications are separate annotations.");
        ImGui::EndChild();
        return;
    }

    if (product.page == ProductPage::patterns) {
        ImGui::SeparatorText("Recurring incident discovery");
        if (ImGui::Button("Refresh recurring groups")) {
            command.action = DashboardAction::refresh_recurring_incidents;
        }
        ImGui::SameLine();
        const auto& recurring = content.recurring;
        ImGui::TextDisabled("%s | %.2f ms", recurring.status.c_str(),
                            recurring.elapsed_milliseconds);
        if (recurring.state == RecurringIncidentViewState::ready) {
            ImGui::TextDisabled("Feature v%d | %zu incidents | %zu cached | %zu recomputed | "
                                "%zu "
                                "noise",
                                recurring.feature_version, recurring.incidents_considered,
                                recurring.cached_features, recurring.recomputed_features,
                                recurring.noise.size());
            for (std::size_t group_index = 0U; group_index < recurring.groups.size();
                 ++group_index) {
                const auto& group = recurring.groups[group_index];
                const auto heading = group.name + " (" + std::to_string(group.members.size()) +
                                     " occurrences)##recurring-" + std::to_string(group_index);
                if (!ImGui::TreeNode(heading.c_str())) continue;
                ImGui::TextWrapped("Shared evidence: %s", group.shared_evidence.c_str());
                ImGui::TextDisabled("%s grouping | maximum member distance %.3f",
                                    group.manually_overridden ? "User-overridden" : "Automatic",
                                    group.maximum_pair_distance);
                for (const auto& member : group.members) {
                    const auto member_id = std::to_string(member.id);
                    ImGui::PushID(member_id.c_str());
                    if (ImGui::SmallButton("Inspect")) {
                        command.action = DashboardAction::select_incident;
                        command.incident_id = member.id;
                        product.page = ProductPage::detail;
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted((member.created_utc + " | " +
                                            (member.label.empty() ? "(unlabeled)" : member.label))
                                               .c_str());
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            if (!recurring.noise.empty() && ImGui::TreeNode("Noise / unique incidents")) {
                for (const auto& member : recurring.noise) {
                    const auto member_id = std::to_string(member.id);
                    ImGui::PushID(member_id.c_str());
                    if (ImGui::SmallButton("Inspect")) {
                        command.action = DashboardAction::select_incident;
                        command.incident_id = member.id;
                        product.page = ProductPage::detail;
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted((member.created_utc + " | " +
                                            (member.label.empty() ? "(unlabeled)" : member.label))
                                               .c_str());
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }

        ImGui::TextWrapped("Groups describe similar recorded evidence. They do "
                           "not prove that one "
                           "process or resource caused each incident.");
        return;
    }

    if (!content.detail) {
        ImGui::SeparatorText("No incident selected");
        ImGui::TextWrapped("Choose a saved incident to inspect its timeline, "
                           "likely contributors, "
                           "and the uncertainty attached to each explanation.");
        if (ImGui::Button("Browse saved incidents")) {
            product.page = ProductPage::incidents;
        }
        ImGui::SameLine();
        if (ImGui::Button("Return to live recorder")) {
            product.page = ProductPage::live;
        }
        return;
    }
    const auto& detail = *content.detail;
    ImGui::SeparatorText("Incident detail");
    if (ImGui::TreeNodeEx("How to read this result", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("Evidence reading guide", 3,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("OBSERVATION");
            ImGui::TextWrapped("Recorded measurements and exact event timing.");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("INFERENCE");
            ImGui::TextWrapped("A statistical explanation that fits those observations.");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("UNCERTAINTY");
            ImGui::TextWrapped("Missing evidence and plausible alternatives remain visible.");
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
    ImGui::BeginChild("Incident explanation", ImVec2{-1.0F, 218.0F}, ImGuiChildFlags_Borders);
    const auto& headline_analysis = detail.analysis;
    if (headline_analysis.pressure.available) {
        ImGui::TextWrapped(
            "OBSERVED PRESSURE: %s via %s (%.0f%%)", headline_analysis.pressure.resource.c_str(),
            headline_analysis.pressure.metric.c_str(), headline_analysis.pressure.score * 100.0);
    } else {
        ImGui::TextWrapped("OBSERVED PRESSURE: No resource cleared the "
                           "practical-effect floor");
    }
    if (headline_analysis.diagnosis.available) {
        ImGui::TextWrapped("SYMPTOM EXPLANATION: %s",
                           headline_analysis.diagnosis.incident_type.c_str());
        ImGui::TextWrapped("LIKELY CONTRIBUTOR: %s",
                           headline_analysis.diagnosis.primary_contributor.empty()
                               ? "No process cleared the evidence threshold"
                               : headline_analysis.diagnosis.primary_contributor.c_str());
        ImGui::Text("UNCERTAINTY: %s (%.0f%% calibrated confidence; %.0f%% "
                    "evidence coverage)",
                    headline_analysis.diagnosis.confidence.c_str(),
                    headline_analysis.diagnosis.calibrated_confidence * 100.0,
                    headline_analysis.diagnosis.evidence_coverage * 100.0);
        if (!headline_analysis.diagnosis.evidence.empty()) {
            ImGui::TextWrapped("PLAIN EVIDENCE: %s",
                               headline_analysis.diagnosis.evidence.front().c_str());
        }
        ImGui::TextDisabled("Basis: %s. Pressure and contributors remain "
                            "correlation, not proof of cause.",
                            headline_analysis.diagnosis.basis.c_str());
    } else {
        ImGui::TextWrapped(headline_analysis.diagnosis.suppressed_by_feedback
                               ? "SYMPTOM EXPLANATION: Unknown - repeated matching triggers "
                                 "were "
                                 "not noticed"
                               : "SYMPTOM EXPLANATION: Unknown - no independent alignment");
        ImGui::TextWrapped("LIKELY CONTRIBUTOR: Not enough evidence to rank reliably");
        ImGui::TextWrapped("UNCERTAINTY: High - inspect the timelines and "
                           "availability notes below");
    }
    if (!headline_analysis.contributors.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted("What stood out");
        const auto visible = std::min<std::size_t>(3U, headline_analysis.contributors.size());
        for (std::size_t index = 0U; index < visible; ++index) {
            const auto& contributor = headline_analysis.contributors[index];
            ImGui::BulletText("%s - %s", contributor.name.c_str(), contributor.assessment.c_str());
        }
    }
    ImGui::Text("Captured %s | requested %.2f to %.2f s | actual %.2f to %.2f s "
                "| triggers %u",
                detail.created_utc.c_str(), detail.requested_start_seconds,
                detail.requested_end_seconds, detail.actual_start_seconds,
                detail.actual_end_seconds, detail.trigger_count);
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::BeginChild("Incident annotations", ImVec2{-1.0F, 300.0F}, ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("Annotation and recurring group");
    ImGui::TextDisabled("Annotations are editable metadata; the captured "
                        "evidence remains immutable.");
    ImGui::TextUnformatted("Label");
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputText("##incident-label", state.label_editor.data(), state.label_editor.size());
    ImGui::TextUnformatted("Note");
    ImGui::InputTextMultiline("##incident-note", state.note_editor.data(), state.note_editor.size(),
                              ImVec2{-1.0F, 70.0F});
    constexpr const char* category_names[] = {"Unknown",      "System freeze",
                                              "Game stutter", "Application slowdown/hang",
                                              "Network",      "Audio"};
    auto category_index = static_cast<int>(state.category_editor);
    ImGui::TextUnformatted("Incident category");
    ImGui::SetNextItemWidth(260.0F);
    if (ImGui::Combo("##incident-category", &category_index, category_names,
                     static_cast<int>(std::size(category_names)))) {
        state.category_editor = static_cast<IncidentCategory>(category_index);
    }
    if (ImGui::Button("Save incident details")) {
        command.action = DashboardAction::save_incident_annotation;
        command.incident_id = detail.id;
        command.label = state.label_editor.data();
        command.note = state.note_editor.data();
        command.incident_feedback = detail.user_feedback;
        command.incident_category = state.category_editor;
    }
    ImGui::TextUnformatted("Recurring group override");
    ImGui::SetNextItemWidth(260.0F);
    ImGui::InputTextWithHint("##recurring-group-override", "Optional user group name",
                             state.recurring_group_override_editor.data(),
                             state.recurring_group_override_editor.size());
    if (ImGui::Button("Save recurring override")) {
        command.action = DashboardAction::save_recurring_group_override;
        command.incident_id = detail.id;
        command.recurring_group_override = state.recurring_group_override_editor.data();
    }
    ImGui::SameLine();
    if (ImGui::Button("Return to automatic grouping")) {
        state.recurring_group_override_editor.fill('\0');
        command.action = DashboardAction::save_recurring_group_override;
        command.incident_id = detail.id;
        command.recurring_group_override.clear();
    }
    ImGui::TextDisabled("Matching non-empty names force incidents into one "
                        "user-overridden group.");
    ImGui::EndChild();

    if (detail.automatic_trigger_count != 0U) {
        ImGui::TextColored(ImVec4{1.0F, 0.75F, 0.25F, 1.0F},
                           "Automatically captured for %s: %s (%u automatic, "
                           "%u manual trigger%s)",
                           automatic_resource_text(detail.automatic_resource),
                           automatic_signal_text(detail.automatic_signal),
                           detail.automatic_trigger_count, detail.manual_trigger_count,
                           detail.manual_trigger_count == 1U ? "" : "s");
        ImGui::TextDisabled("Observed %.3g | baseline %.3g | detector score %.2f",
                            detail.automatic_observed_value, detail.automatic_baseline_value,
                            detail.automatic_score);
        if (detail.user_feedback == IncidentFeedback::unanswered) {
            ImGui::TextUnformatted("Did you notice a problem at this time?");
            if (ImGui::Button("Yes, I noticed a problem")) {
                command.action = DashboardAction::save_incident_feedback;
                command.incident_id = detail.id;
                command.label = state.label_editor.data();
                command.note = state.note_editor.data();
                command.incident_feedback = IncidentFeedback::noticed_problem;
                command.incident_category = state.category_editor;
            }
            ImGui::SameLine();
            if (ImGui::Button("No, I did not notice one")) {
                command.action = DashboardAction::save_incident_feedback;
                command.incident_id = detail.id;
                command.label = state.label_editor.data();
                command.note = state.note_editor.data();
                command.incident_feedback = IncidentFeedback::did_not_notice_problem;
                command.incident_category = state.category_editor;
            }
        } else {
            ImGui::TextDisabled("Feedback retained: %s",
                                detail.user_feedback == IncidentFeedback::noticed_problem
                                    ? "problem noticed"
                                    : "no problem noticed");
        }
    }

    ImGui::Spacing();
    if (!ImGui::CollapsingHeader("Inspect timelines, factors, and raw evidence")) {
        return;
    }
    ImGui::SeparatorText("Potential contributors");
    const auto& analysis = detail.analysis;
    if (analysis.state == IncidentAnalysisViewState::disabled) {
        ImGui::TextDisabled("Analysis disabled; recording and viewing are unchanged.");
    } else if (analysis.state == IncidentAnalysisViewState::error) {
        ImGui::TextColored(ImVec4{1.0F, 0.45F, 0.35F, 1.0F}, "%s", analysis.status.c_str());
    } else {
        ImGui::TextWrapped("%s", analysis.status.c_str());
        ImGui::SeparatorText("Observed resource pressure");
        if (analysis.pressure.available) {
            ImGui::Text("%s via %s - %.1f%% practical pressure score",
                        analysis.pressure.resource.c_str(), analysis.pressure.metric.c_str(),
                        analysis.pressure.score * 100.0);
            ImGui::TextWrapped("%s", analysis.pressure.evidence.c_str());
            ImGui::TextDisabled("This pressure was measured near the marker. It may "
                                "be unrelated to the reported symptom.");
        } else {
            ImGui::TextDisabled("No resource metric cleared its practical-effect "
                                "floor. Raw statistical evidence remains below.");
        }
        ImGui::SeparatorText("Symptom explanation");
        if (analysis.diagnosis.pipeline_version == 0U) {
            ImGui::TextDisabled("This analyzer does not emit the versioned explanation model.");
        } else if (!analysis.diagnosis.available) {
            ImGui::TextDisabled(analysis.diagnosis.suppressed_by_feedback
                                    ? "Unknown: bounded local feedback suppressed this "
                                      "automatic-trigger assertion. Raw evidence remains "
                                      "inspectable "
                                      "below."
                                    : "Unknown: observed pressure did not align with an "
                                      "independent "
                                      "symptom signal. Evidence remains inspectable below.");
            ImGui::TextDisabled(
                "Pipeline v%u / evidence v%u / configuration %016llx | %s",
                analysis.diagnosis.pipeline_version, analysis.diagnosis.evidence_model_version,
                static_cast<unsigned long long>(analysis.diagnosis.configuration_fingerprint),
                analysis.diagnosis.inference.c_str());
        } else {
            ImGui::Text("%s - %s (%.1f%% calibrated)", analysis.diagnosis.incident_type.c_str(),
                        analysis.diagnosis.confidence.c_str(),
                        analysis.diagnosis.calibrated_confidence * 100.0);
            ImGui::TextWrapped("Explanation basis: %s", analysis.diagnosis.basis.c_str());
            ImGui::TextWrapped("This is a probabilistic local symptom explanation, "
                               "not proof of cause. Every factor below "
                               "references recorded evidence; correlated factors are "
                               "explicitly penalized.");
            if (!analysis.diagnosis.primary_contributor.empty()) {
                ImGui::TextWrapped("Highest aligned preceding contributor: %s (correlation "
                                   "only)",
                                   analysis.diagnosis.primary_contributor.c_str());
            }
            ImGui::TextDisabled(
                "Evidence coverage %.0f%% | correlated-evidence penalty %.1f%% "
                "| "
                "pipeline v%u / evidence v%u / configuration %016llx | %s",
                analysis.diagnosis.evidence_coverage * 100.0,
                analysis.diagnosis.correlated_evidence_penalty * 100.0,
                analysis.diagnosis.pipeline_version, analysis.diagnosis.evidence_model_version,
                static_cast<unsigned long long>(analysis.diagnosis.configuration_fingerprint),
                analysis.diagnosis.inference.c_str());
            for (const auto& evidence : analysis.diagnosis.evidence) {
                ImGui::BulletText("%s", evidence.c_str());
            }
        }
        if (analysis.feedback.applicable) {
            ImGui::SeparatorText("Local feedback calibration");
            ImGui::TextWrapped("%s", analysis.feedback.status.c_str());
        }
        if (analysis.similar_incidents.applicable) {
            ImGui::SeparatorText("Confirmed similar incidents");
            ImGui::TextWrapped("%s", analysis.similar_incidents.status.c_str());
            if (analysis.similar_incidents.ready) {
                render_disabled_wrapped("Historical context only. It does not change this "
                                        "diagnosis, "
                                        "confidence, contributor ranking, or recorded evidence.");
            } else if (analysis.similar_incidents.manually_excluded) {
                render_disabled_wrapped("Manual grouping remains available for "
                                        "navigation but cannot teach "
                                        "the analysis pipeline.");
            } else {
                render_disabled_wrapped("Sparse or conflicting feedback is retained "
                                        "for inspection and is not reused.");
            }
        }
        if (analysis.feedback.applicable || analysis.similar_incidents.applicable) {
            ImGui::SeparatorText("Feedback influence controls");
            ImGui::TextDisabled("Profile revision %llu%s",
                                static_cast<unsigned long long>(analysis.feedback.profile_revision),
                                analysis.feedback.reset_after_utc_milliseconds > 0
                                    ? " | reset baseline active"
                                    : "");
            ImGui::Checkbox("Confirm reset of learned feedback influence",
                            &product.feedback_reset_confirmed);
            const bool feedback_reset_disabled = !product.feedback_reset_confirmed;
            if (feedback_reset_disabled) ImGui::BeginDisabled();
            if (ImGui::Button("Reset feedback influence")) {
                command.action = DashboardAction::reset_feedback_profile;
                product.feedback_reset_confirmed = false;
            }
            if (feedback_reset_disabled) ImGui::EndDisabled();
            if (analysis.feedback.rollback_available) {
                ImGui::SameLine();
                if (ImGui::Button("Undo last reset")) {
                    command.action = DashboardAction::rollback_feedback_profile_reset;
                    product.feedback_reset_confirmed = false;
                }
            }
            render_disabled_wrapped("Automatic-trigger calibration uses exact "
                                    "resource/signal matches. "
                                    "Similar-incident context uses only "
                                    "bounded prior confirmations from "
                                    "automatic recurrence groups.");
            ImGui::TextWrapped("Resetting stops older feedback from "
                               "influencing future diagnoses or "
                               "historical context. "
                               "It preserves every incident, annotation, and "
                               "recorded sample.");
        }
        ImGui::SeparatorText("Probabilistic workload context");
        if (!analysis.context.enabled) {
            ImGui::TextDisabled("Context recognition is disabled; anomaly "
                                "scores are unadjusted.");
        } else {
            ImGui::Text("%s (%.0f%% probability, %.0f%% uncertainty)",
                        analysis.context.primary.c_str(), analysis.context.confidence * 100.0,
                        analysis.context.uncertainty * 100.0);
            ImGui::TextWrapped("Context is an uncertain local heuristic, not a "
                               "record of user intent. "
                               "It can only softly reduce workload-expected anomaly "
                               "ranks; raw evidence remains visible.");
            if (ImGui::BeginTable("Context probabilities", 2,
                                  ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Workload");
                ImGui::TableSetupColumn("Probability");
                ImGui::TableHeadersRow();
                for (const auto& probability : analysis.context.probabilities) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(probability.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.1f%%", probability.probability * 100.0);
                }
                ImGui::EndTable();
            }
            for (const auto& evidence : analysis.context.evidence) {
                ImGui::BulletText("%s", evidence.c_str());
            }
        }
        ImGui::TextWrapped("Contributor ranks combine correlation evidence; they "
                           "do not prove causation. "
                           "Marker-spanning activity is ambiguous; post-marker "
                           "activity is shown as a possible "
                           "victim or reaction, not a causal rank.");
        render_disabled_wrapped("Attribution is an explicit causal judgment, "
                                "separate from whether you noticed a problem. "
                                "It affects only future exact executable/resource "
                                "matches after four consistent incidents. "
                                "Positive learning requires that the attributed "
                                "activity genuinely preceded its marker.");
        if (analysis.contributors.empty()) {
            ImGui::TextDisabled("No process has sufficient anomaly evidence for "
                                "contributor ranking.");
        } else if (ImGui::BeginTable("Contributor ranking", 5,
                                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Process");
            ImGui::TableSetupColumn("Assessment");
            ImGui::TableSetupColumn("Timing");
            ImGui::TableSetupColumn("Inspectable evidence");
            ImGui::TableSetupColumn("Your attribution");
            ImGui::TableHeadersRow();
            for (std::size_t index = 0U; index < analysis.contributors.size(); ++index) {
                const auto& row = analysis.contributors[index];
                ImGui::PushID(static_cast<int>(index));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s (%.0f%%)", row.name.c_str(), row.score * 100.0);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextWrapped("%s", row.assessment.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped("%s", row.timing.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextWrapped("%s", row.evidence.c_str());
                if (row.feedback_multiplier != 1.0) {
                    char calibration[128]{};
                    std::snprintf(calibration, sizeof(calibration),
                                  "Incident-local %.0f%% -> calibrated %.0f%% (x%.3f)",
                                  row.score_before_feedback * 100.0, row.score * 100.0,
                                  row.feedback_multiplier);
                    render_disabled_wrapped(calibration);
                }
                ImGui::TableSetColumnIndex(4);
                constexpr const char* attribution_names[] = {"Unsure", "Confirmed contributor",
                                                             "Not a contributor"};
                auto attribution = static_cast<int>(row.attribution);
                ImGui::BeginDisabled(row.executable_key.empty());
                ImGui::SetNextItemWidth(-1.0F);
                if (ImGui::Combo("##ContributorAttribution", &attribution, attribution_names,
                                 static_cast<int>(std::size(attribution_names)))) {
                    command.action = DashboardAction::save_contributor_feedback;
                    command.incident_id = detail.id;
                    command.contributor_executable_key = row.executable_key;
                    command.contributor_resource = row.resource;
                    command.contributor_attribution =
                        static_cast<IncidentContributorRow::Attribution>(attribution);
                    command.contributor_temporal_relationship = row.temporal_relationship;
                }
                ImGui::EndDisabled();
                render_disabled_wrapped(row.feedback_status.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Statistical and personalized anomaly ranking");
        ImGui::TextDisabled("Baseline %.1f to %.1f s | evaluation %.1f to %.1f s | missing "
                            "values "
                            "%zu",
                            analysis.baseline_start_seconds, analysis.baseline_end_seconds,
                            analysis.evaluation_start_seconds, analysis.evaluation_end_seconds,
                            analysis.missing_values);
        ImGui::TextDisabled("Scores describe unusual behavior, not proven "
                            "causation. Confidence reflects baseline coverage.");
        const auto render_ranking = [](const char* table_id,
                                       const std::vector<IncidentAnomalyRow>& rows,
                                       const float height) {
            if (ImGui::BeginTable(table_id, 4,
                                  ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                                  ImVec2{-1.0F, height})) {
                ImGui::TableSetupColumn("Candidate", ImGuiTableColumnFlags_WidthFixed, 180.0F);
                ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 75.0F);
                ImGui::TableSetupColumn("Confidence", ImGuiTableColumnFlags_WidthFixed, 125.0F);
                ImGui::TableSetupColumn("Strongest evidence", ImGuiTableColumnFlags_WidthStretch,
                                        1.0F);
                ImGui::TableHeadersRow();
                for (const auto& row : rows) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(row.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.1f%%", row.score * 100.0);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(row.confidence.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextWrapped("%s", row.evidence.c_str());
                }
                ImGui::EndTable();
            }
        };
        render_ranking("Resource anomaly ranking", analysis.resources, 130.0F);
        if (!analysis.processes.empty()) {
            ImGui::TextDisabled("Top process candidates (personalized when history "
                                "is ready; bounded to %zu)",
                                incident_analysis_process_capacity);
            render_ranking("Process anomaly ranking", analysis.processes, 190.0F);
        }
    }

    const auto x_min = detail.actual_start_seconds < detail.actual_end_seconds
                           ? detail.actual_start_seconds
                           : detail.actual_start_seconds - 1.0;
    const auto x_max = detail.actual_start_seconds < detail.actual_end_seconds
                           ? detail.actual_end_seconds
                           : detail.actual_end_seconds + 1.0;
    if (!product.timeline_initialized || product.timeline_incident_id != detail.id) {
        product.timeline_initialized = true;
        product.timeline_incident_id = detail.id;
        product.timeline_min = x_min;
        product.timeline_max = x_max;
        clear_timeline_cursor(product);
    }
    if (ImGui::Button("Reset timeline zoom")) {
        product.timeline_min = x_min;
        product.timeline_max = x_max;
    }
    if (product.timeline_cursor_visible) {
        ImGui::SameLine();
        if (ImGui::Button("Clear synchronized cursor")) {
            clear_timeline_cursor(product);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Pan, wheel-zoom, or hover any timeline; zoom and cursor "
                        "stay synchronized.");
    if (product.timeline_cursor_visible) {
        ImGui::Text("Synchronized cursor: %+.2f s from incident", product.timeline_cursor_seconds);
    } else {
        ImGui::TextDisabled("Hover a timeline to inspect the same moment across every plot.");
    }
    if (ImPlot::BeginPlot("System utilization", ImVec2{-1.0F, 210.0F})) {
        ImPlot::SetupAxes("Seconds from event", "Percent");
        ImPlot::SetupAxisLinks(ImAxis_X1, &product.timeline_min, &product.timeline_max);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 100.0, ImGuiCond_Always);
        render_series("CPU", detail.cpu_percent);
        render_series("Memory", detail.memory_percent);
        render_timeline_references(product, x_min, x_max);
        ImPlot::EndPlot();
    }
    render_missing_summary("CPU", detail.cpu_percent);
    render_missing_summary("Memory", detail.memory_percent);

    if (ImPlot::BeginPlot("System throughput", ImVec2{-1.0F, 210.0F})) {
        ImPlot::SetupAxes("Seconds from event", "MiB/s");
        ImPlot::SetupAxisLinks(ImAxis_X1, &product.timeline_min, &product.timeline_max);
        render_series("Disk read", detail.disk_read_mib_per_second);
        render_series("Disk write", detail.disk_write_mib_per_second);
        render_series("Network receive", detail.network_receive_mib_per_second);
        render_series("Network transmit", detail.network_transmit_mib_per_second);
        render_timeline_references(product, x_min, x_max);
        ImPlot::EndPlot();
    }
    render_missing_summary("Disk read", detail.disk_read_mib_per_second);
    render_missing_summary("Disk write", detail.disk_write_mib_per_second);
    render_missing_summary("Network receive", detail.network_receive_mib_per_second);
    render_missing_summary("Network transmit", detail.network_transmit_mib_per_second);

    ImGui::TextWrapped("Physical-disk quality is measured below the process I/O layer. A "
                       "process doing I/O may correlate with a stall, but its byte count does "
                       "not prove it caused device latency or queueing.");
    if (ImPlot::BeginPlot("Physical storage quality", ImVec2{-1.0F, 210.0F})) {
        ImPlot::SetupAxes("Seconds from event", "Milliseconds / requests");
        ImPlot::SetupAxisLinks(ImAxis_X1, &product.timeline_min, &product.timeline_max);
        render_series("Read latency ms", detail.disk_read_latency_milliseconds);
        render_series("Write latency ms", detail.disk_write_latency_milliseconds);
        render_series("Service time ms", detail.disk_service_time_milliseconds);
        render_series("Queue depth", detail.disk_queue_depth);
        render_timeline_references(product, x_min, x_max);
        ImPlot::EndPlot();
    }
    render_missing_summary("Physical-disk latency", detail.disk_service_time_milliseconds);
    render_missing_summary("Physical-disk queue", detail.disk_queue_depth);

    ImGui::TextWrapped("Network quality is passive, host-wide evidence. Connectivity state, "
                       "interface transitions, TCP retransmissions, failed opens, and resets "
                       "do "
                       "not identify an application, remote endpoint, physical packet loss, "
                       "DNS "
                       "failure, or payload cause. BlackBox sends no probe traffic and does "
                       "not "
                       "claim RTT latency.");
    if (ImPlot::BeginPlot("Network connectivity and transport quality", ImVec2{-1.0F, 210.0F})) {
        ImPlot::SetupAxes("Seconds from event", "Level / percent / events");
        ImPlot::SetupAxisLinks(ImAxis_X1, &product.timeline_min, &product.timeline_max);
        render_series("Connectivity (0 none, 1 local, 2 internet, 3 "
                      "constrained, 4 unknown)",
                      detail.network_connectivity_level);
        render_series("TCP retransmit %", detail.network_tcp_retransmit_percent);
        render_series("Interface transitions", detail.network_interface_changes);
        render_series("Failed TCP connections", detail.network_tcp_failed_connections);
        render_series("Reset TCP connections", detail.network_tcp_resets);
        render_timeline_references(product, x_min, x_max);
        ImPlot::EndPlot();
    }
    render_missing_summary("Connectivity", detail.network_connectivity_level);
    render_missing_summary("TCP retransmission", detail.network_tcp_retransmit_percent);

    ImGui::TextWrapped("GPU utilization is the busiest physical engine, not a sum across "
                       "engines. A foreground GPU drop may describe a victim waiting for "
                       "work; "
                       "foreground identity and timing are correlations, never causal proof.");
    if (ImPlot::BeginPlot("GPU engine and memory evidence", ImVec2{-1.0F, 210.0F})) {
        ImPlot::SetupAxes("Seconds from event", "Percent / MiB");
        ImPlot::SetupAxisLinks(ImAxis_X1, &product.timeline_min, &product.timeline_max);
        render_series("Busiest GPU engine %", detail.gpu_percent);
        render_series("Foreground GPU engine %", detail.foreground_gpu_percent);
        render_series("Dedicated GPU memory MiB", detail.gpu_dedicated_memory_mib);
        render_series("Shared GPU memory MiB", detail.gpu_shared_memory_mib);
        render_timeline_references(product, x_min, x_max);
        ImPlot::EndPlot();
    }
    render_missing_summary("GPU engine", detail.gpu_percent);
    render_missing_summary("GPU memory", detail.gpu_dedicated_memory_mib);

    ImGui::TextWrapped("Where the platform exposes them, DPC and interrupt time can show "
                       "pressure on time-sensitive audio or rendering threads, but aggregate "
                       "activity cannot identify a driver. CPU thermal evidence is "
                       "platform-specific context; a limit or pressure state is not a "
                       "temperature or root-cause claim.");
    if (ImPlot::BeginPlot("Responsiveness and power evidence", ImVec2{-1.0F, 210.0F})) {
        ImPlot::SetupAxes("Seconds from event", "Percent / MHz / DPC rate");
        ImPlot::SetupAxisLinks(ImAxis_X1, &product.timeline_min, &product.timeline_max);
        render_series("DPC time %", detail.dpc_percent);
        render_series("Interrupt time %", detail.interrupt_percent);
        render_series("DPC rate", detail.dpc_rate);
        render_series("CPU current MHz", detail.cpu_current_mhz);
        render_series("CPU thermal limit MHz", detail.cpu_thermal_limit_mhz);
        render_series("CPU thermal limit %", detail.cpu_thermal_limit_percent);
        render_series("Battery %", detail.battery_percent);
        render_timeline_references(product, x_min, x_max);
        ImPlot::EndPlot();
    }
    render_missing_summary("DPC/ISR", detail.dpc_percent);
    render_missing_summary("CPU thermal limit", detail.cpu_thermal_limit_percent);

    ImGui::TextWrapped("Pressure is exact interval stall time where the platform "
                       "exposes cumulative counters; it is not utilization or "
                       "proof of a cause. Thermal pressure is a separate coarse "
                       "platform state and is never relabeled as PSI.");
    if (ImPlot::BeginPlot("Resource pressure context", ImVec2{-1.0F, 210.0F})) {
        ImPlot::SetupAxes("Seconds from event", "Stalled interval % / thermal state");
        ImPlot::SetupAxisLinks(ImAxis_X1, &product.timeline_min, &product.timeline_max);
        render_series("CPU some %", detail.cpu_some_pressure_percent);
        render_series("Memory some %", detail.memory_some_pressure_percent);
        render_series("Memory full %", detail.memory_full_pressure_percent);
        render_series("I/O some %", detail.io_some_pressure_percent);
        render_series("I/O full %", detail.io_full_pressure_percent);
        render_series("Thermal state (0 nominal - 3 critical)", detail.thermal_pressure_state);
        render_timeline_references(product, x_min, x_max);
        ImPlot::EndPlot();
    }
    render_missing_summary("CPU stall pressure", detail.cpu_some_pressure_percent);
    render_missing_summary("Memory stall pressure", detail.memory_some_pressure_percent);
    render_missing_summary("I/O stall pressure", detail.io_some_pressure_percent);
    render_missing_summary("Thermal pressure state", detail.thermal_pressure_state);

    ImGui::SeparatorText("Foreground and system-event evidence");
    ImGui::TextDisabled("The recorder never stores window titles, Event Log messages/payloads, "
                        "device/audio IDs, or storage addresses.");
    if (!detail.foreground_applications.empty() &&
        ImGui::BeginTable("Foreground transitions", 3,
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 75.0F);
        ImGui::TableSetupColumn("Foreground application");
        ImGui::TableSetupColumn("GPU", ImGuiTableColumnFlags_WidthFixed, 80.0F);
        ImGui::TableHeadersRow();
        for (const auto& row : detail.foreground_applications) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%+.2f s", row.seconds_from_event);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s (PID %u)", row.name.c_str(), row.identity.pid);
            ImGui::TableSetColumnIndex(2);
            row.gpu_available ? ImGui::Text("%.1f%%", row.gpu_percent) : ImGui::TextDisabled("N/A");
        }
        ImGui::EndTable();
    }
    if (detail.system_events.empty()) {
        ImGui::TextDisabled("No enabled system-event source delivered an event "
                            "in this window.");
    } else if (ImGui::BeginTable("System event evidence", 4,
                                 ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollY,
                                 ImVec2{-1.0F, 180.0F})) {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 75.0F);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 150.0F);
        ImGui::TableSetupColumn("Event");
        ImGui::TableSetupColumn("Level / ID", ImGuiTableColumnFlags_WidthFixed, 105.0F);
        ImGui::TableHeadersRow();
        for (const auto& row : detail.system_events) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%+.2f s", row.seconds_from_event);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(row.source.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(row.event.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%s / %u", row.level.c_str(), row.native_event_id);
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Incident processes");
    auto process_view_changed =
        ImGui::InputTextWithHint("##incident-process-filter", "Filter name, path, or PID",
                                 state.process_filter.data(), state.process_filter.size());
    ImGui::SameLine();
    constexpr const char* process_sort_names[]{"Name",        "PID",       "Peak CPU",
                                               "Peak memory", "Peak read", "Peak write"};
    auto process_sort_index = static_cast<int>(state.process_sort);
    ImGui::SetNextItemWidth(130.0F);
    process_view_changed =
        ImGui::Combo("##incident-process-sort", &process_sort_index, process_sort_names,
                     static_cast<int>(std::size(process_sort_names))) ||
        process_view_changed;
    state.process_sort = static_cast<IncidentProcessSort>(process_sort_index);
    ImGui::SameLine();
    process_view_changed =
        ImGui::Checkbox("Ascending", &state.process_sort_ascending) || process_view_changed;
    if (process_view_changed || state.visible_process_indices.empty()) {
        state.visible_process_indices =
            filter_and_sort_processes(detail.processes, state.process_filter.data(),
                                      state.process_sort, state.process_sort_ascending);
    }
    ImGui::TextDisabled("Showing %zu of %zu identities (bounded to %zu)",
                        state.visible_process_indices.size(), detail.processes.size(),
                        incident_process_visible_capacity);
    if (ImGui::BeginTable("Incident process table", 7,
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2{-1.0F, 230.0F})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Process");
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 92.0F);
        ImGui::TableSetupColumn("Samples", ImGuiTableColumnFlags_WidthFixed, 70.0F);
        ImGui::TableSetupColumn("Peak CPU", ImGuiTableColumnFlags_WidthFixed, 80.0F);
        ImGui::TableSetupColumn("Peak MiB", ImGuiTableColumnFlags_WidthFixed, 90.0F);
        ImGui::TableSetupColumn("Read MiB/s", ImGuiTableColumnFlags_WidthFixed, 90.0F);
        ImGui::TableSetupColumn("Write MiB/s", ImGuiTableColumnFlags_WidthFixed, 90.0F);
        ImGui::TableHeadersRow();
        for (const auto index : state.visible_process_indices) {
            if (index >= detail.processes.size()) continue;
            const auto& process = detail.processes[index];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const auto id = process.name + "##process-" + std::to_string(process.identity.pid) +
                            "-" + std::to_string(process.identity.creation_token);
            const auto selected =
                detail.selected_process && *detail.selected_process == process.identity;
            if (ImGui::Selectable(id.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                command.action = DashboardAction::select_incident_process;
                command.incident_id = detail.id;
                command.process_identity = process.identity;
            }
            if (!process.executable_path.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", process.executable_path.c_str());
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", process.identity.pid);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%zu", process.sample_count);
            ImGui::TableSetColumnIndex(3);
            process.cpu_available ? ImGui::Text("%.1f%%", process.peak_cpu_percent)
                                  : ImGui::TextDisabled("N/A");
            ImGui::TableSetColumnIndex(4);
            process.working_set_available
                ? render_compact_decimal(process.peak_working_set_mib, "%.1f")
                : ImGui::TextDisabled("N/A");
            ImGui::TableSetColumnIndex(5);
            process.disk_read_available
                ? render_compact_decimal(process.peak_disk_read_mib_per_second, "%.2f")
                : ImGui::TextDisabled("N/A");
            ImGui::TableSetColumnIndex(6);
            process.disk_write_available
                ? render_compact_decimal(process.peak_disk_write_mib_per_second, "%.2f")
                : ImGui::TextDisabled("N/A");
        }
        ImGui::EndTable();
    }
    if (detail.selected_process) {
        const auto title =
            "Selected process timeline (PID " + std::to_string(detail.selected_process->pid) + ")";
        if (ImPlot::BeginPlot(title.c_str(), ImVec2{-1.0F, 200.0F})) {
            ImPlot::SetupAxes("Seconds from event", "CPU % / MiB / MiB/s");
            ImPlot::SetupAxisLinks(ImAxis_X1, &product.timeline_min, &product.timeline_max);
            render_series("CPU %", detail.selected_process_cpu_percent);
            render_series("Working set MiB", detail.selected_process_working_set_mib);
            render_series("Disk read MiB/s", detail.selected_process_disk_read_mib_per_second);
            render_series("Disk write MiB/s", detail.selected_process_disk_write_mib_per_second);
            render_timeline_references(product, x_min, x_max);
            ImPlot::EndPlot();
        }
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
                ImGui::TextWrapped("BlackBox records a short, bounded resource "
                                   "history. Collection "
                                   "stays independent from this window and "
                                   "from later analysis.");
                ImGui::SeparatorText("2  Capture the moment");
                ImGui::TextWrapped("When something feels wrong, use Capture "
                                   "incident to preserve the "
                                   "moments before and after it. Configure the "
                                   "global shortcut in "
                                   "Settings when the platform supports one.");
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
                ImGui::TextDisabled("Keyboard: Tab and Shift+Tab move focus; "
                                    "Enter or Space activates "
                                    "a control. After setup, Ctrl+1 through "
                                    "Ctrl+6 changes pages.");
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
            if (ImGui::BeginChild("Recorder summary", ImVec2{0.0F, 164.0F},
                                  ImGuiChildFlags_Borders)) {
                ImGui::TextDisabled("RECORDER STATUS");
                if (recording && state.incident_capture_enabled) {
                    ImGui::TextColored(ui_color(visual.success),
                                       "Recording and ready to capture what just happened");
                } else if (recording) {
                    ImGui::TextColored(ui_color(visual.warning), "Recording; incident capture is "
                                                                 "temporarily unavailable");
                } else {
                    ImGui::TextColored(ui_color(visual.warning), "Recorder is stopped");
                }
                ImGui::TextDisabled("Hotkey %s  |  Archive %s", state.hotkey_status.c_str(),
                                    state.storage_status.c_str());
                ImGui::TextDisabled("Background %s", state.background_status.c_str());
                if (!state.incident_capture_enabled) ImGui::BeginDisabled();
                ImGui::PushStyleColor(ImGuiCol_Button, ui_color(visual.accent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui_color(visual.accent_hovered));
                if (ImGui::Button("Capture what just happened", ImVec2{250.0F, 34.0F})) {
                    command.action = DashboardAction::capture_incident;
                }
                ImGui::PopStyleColor(2);
                if (!state.incident_capture_enabled) ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextUnformatted(state.incident_capture_status.c_str());
                ImGui::TextDisabled("%llu saved incident%s",
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
            render_incident_viewer(incident_viewer, command, product);
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
            ImGui::SeparatorText("Live system telemetry");
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
                render_metric_card("CPU card", "TOTAL CPU", state.cpu_status, state.cpu_usage,
                                   cpu_overlay);
                ImGui::TableNextColumn();
                render_metric_card("Memory card", "PHYSICAL MEMORY", state.memory_status,
                                   state.memory_usage, memory_overlay);
                ImGui::EndTable();
            }
            if (ImGui::BeginTable("Live throughput", 2,
                                  ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthFixed, 180.0F);
                ImGui::TableSetupColumn("Rate");
                render_rate_row("Disk read", state.disk_read_status,
                                state.disk_read_mib_per_second);
                render_rate_row("Disk write", state.disk_write_status,
                                state.disk_write_mib_per_second);
                render_rate_row("Network receive", state.network_receive_status,
                                state.network_receive_mib_per_second);
                render_rate_row("Network transmit", state.network_transmit_status,
                                state.network_transmit_mib_per_second);
                ImGui::EndTable();
            }
            if (ImGui::CollapsingHeader("Forensic telemetry details")) {
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
                        ImGui::Text("PID %u | foreground GPU %.1f%%", state.foreground_pid,
                                    state.foreground_gpu_usage * 100.0);
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
            if (ImGui::CollapsingHeader("Rolling history")) {
                if (state.history_size == 0U) {
                    ImGui::TextDisabled("Waiting for recorder samples");
                } else if (ImPlot::BeginPlot("##system-history", ImVec2{-1.0F, 220.0F},
                                             ImPlotFlags_NoMouseText)) {
                    ImPlot::SetupAxes("Oldest to newest sample", "Utilization (%)",
                                      ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
                    const auto newest_sample = state.history_size > 1U
                                                   ? static_cast<double>(state.history_size - 1U)
                                                   : 1.0;
                    ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, newest_sample, ImGuiCond_Always);
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

                const auto newest_sample =
                    state.history_size > 1U ? static_cast<double>(state.history_size - 1U) : 1.0;
                if (state.disk_read_history_points != 0U || state.disk_write_history_points != 0U) {
                    if (ImPlot::BeginPlot("Disk throughput", ImVec2{-1.0F, 180.0F},
                                          ImPlotFlags_NoMouseText)) {
                        ImPlot::SetupAxes("Oldest to newest sample", "MiB/s",
                                          ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
                        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, newest_sample, ImGuiCond_Always);
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
                    if (ImPlot::BeginPlot("Network throughput", ImVec2{-1.0F, 180.0F},
                                          ImPlotFlags_NoMouseText)) {
                        ImPlot::SetupAxes("Oldest to newest sample", "MiB/s",
                                          ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
                        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, newest_sample, ImGuiCond_Always);
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
            if (ImGui::CollapsingHeader("Active processes (highest CPU first)")) {
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
