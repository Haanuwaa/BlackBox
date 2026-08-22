#include "ui/dashboard.hpp"

#include "core/version.hpp"

#include <imgui.h>
#include <implot.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace blackbox::ui {
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
        return "Temporarily unavailable";
    }
    return "Unknown";
}

void render_metric_unavailable(const MetricDisplayStatus status) {
    ImGui::TextDisabled("%s", status_text(status));
}

void render_disabled_wrapped(const char* const text) {
    ImGui::PushStyleColor(
        ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void render_rate_row(const char* label, const MetricDisplayStatus status,
                     const double value) {
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

void render_timeline_references(ProductUiState& product,
                                const double incident_start_seconds,
                                const double incident_end_seconds) {
    if (ImPlot::IsPlotHovered()) {
        static_cast<void>(set_timeline_cursor(
            product, ImPlot::GetPlotMousePos(ImAxis_X1, ImAxis_Y1).x,
            incident_start_seconds, incident_end_seconds));
    }
    render_event_marker();
    if (!product.timeline_cursor_visible) return;
    ImPlotSpec style{};
    style.LineColor = ImVec4{0.20F, 0.80F, 1.0F, 0.95F};
    style.LineWeight = 1.5F;
    ImPlot::PlotInfLines("Synchronized cursor", &product.timeline_cursor_seconds, 1,
                         style);
}

void render_missing_summary(const char* label, const IncidentPlotSeries& series) {
    if (series.values.empty()) {
        ImGui::TextDisabled("%s unavailable (unsupported %zu, inaccessible %zu, temporary %zu)",
                            label, series.availability.by_status[1],
                            series.availability.by_status[2],
                            series.availability.by_status[3]);
    }
}

[[nodiscard]] constexpr const char* automatic_resource_text(
    const core::AutomaticIncidentResource resource) noexcept {
    switch (resource) {
    case core::AutomaticIncidentResource::none: return "unknown resource";
    case core::AutomaticIncidentResource::cpu: return "CPU";
    case core::AutomaticIncidentResource::memory: return "memory";
    case core::AutomaticIncidentResource::disk: return "storage";
    case core::AutomaticIncidentResource::network: return "network";
    }
    return "unknown resource";
}

[[nodiscard]] constexpr const char* automatic_signal_text(
    const core::AutomaticIncidentSignal signal) noexcept {
    switch (signal) {
    case core::AutomaticIncidentSignal::throughput_or_utilization:
        return "throughput/utilization";
    case core::AutomaticIncidentSignal::disk_latency: return "physical-disk latency";
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
        return "Windows Application Error event";
    case core::AutomaticIncidentSignal::application_hang:
        return "Windows Application Hang event";
    case core::AutomaticIncidentSignal::display_driver_recovery:
        return "Windows display timeout recovery event";
    case core::AutomaticIncidentSignal::storage_io_retry:
        return "Windows storage I/O retry event";
    }
    return "unknown signal";
}

[[nodiscard]] constexpr const char* connectivity_text(
    const std::uint8_t level) noexcept {
    switch (level) {
    case 0U: return "Disconnected";
    case 1U: return "Local access";
    case 2U: return "Internet access";
    case 3U: return "Constrained internet";
    default: return "Unknown";
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
    ImGui::SetNextItemWidth(260.0F);
    ImGui::InputTextWithHint("##incident-search", "Search labels and notes",
                             state.search.data(), state.search.size());
    ImGui::SameLine();
    if (ImGui::Button("Search / refresh")) request_page(command, state, 0U);
    ImGui::SameLine();
    constexpr const char* order_names[]{"Newest", "Oldest", "Longest", "Shortest",
                                         "Label A-Z", "Label Z-A"};
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
                        content.last_build_milliseconds,
                        content.last_analysis_milliseconds);

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
        request_page(command, state, content.offset > page_size
                                         ? content.offset - page_size
                                         : 0U);
    }
    if (content.offset != 0U) ImGui::SameLine();
    if (content.offset + content.incidents.size() < content.total_matching &&
        ImGui::Button("Next page")) {
        request_page(command, state, content.offset + page_size);
    }

    ImGui::TextDisabled(
        "Saved incidents are immutable evidence; labels, notes, and classifications are separate annotations.");
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
        ImGui::TextDisabled(
            "Feature v%d | %zu incidents | %zu cached | %zu recomputed | %zu noise",
            recurring.feature_version, recurring.incidents_considered,
            recurring.cached_features, recurring.recomputed_features,
            recurring.noise.size());
        for (std::size_t group_index = 0U; group_index < recurring.groups.size();
             ++group_index) {
            const auto& group = recurring.groups[group_index];
            const auto heading = group.name + " (" +
                std::to_string(group.members.size()) + " occurrences)##recurring-" +
                std::to_string(group_index);
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
                    (member.label.empty() ? "(unlabeled)" : member.label)).c_str());
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
                    (member.label.empty() ? "(unlabeled)" : member.label)).c_str());
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }

    ImGui::TextWrapped(
        "Groups describe similar recorded evidence. They do not prove that one process or resource caused each incident.");
    return;
    }

    if (!content.detail) {
        ImGui::TextDisabled("Select an incident to inspect its timeline and processes.");
        return;
    }
    const auto& detail = *content.detail;
    ImGui::SeparatorText("Incident detail");
    const auto& headline_analysis = detail.analysis;
    if (headline_analysis.pressure.available) {
        ImGui::TextWrapped("OBSERVED PRESSURE: %s via %s (%.0f%%)",
                           headline_analysis.pressure.resource.c_str(),
                           headline_analysis.pressure.metric.c_str(),
                           headline_analysis.pressure.score * 100.0);
    } else {
        ImGui::TextWrapped("OBSERVED PRESSURE: No resource cleared the practical-effect floor");
    }
    if (headline_analysis.diagnosis.available) {
        ImGui::TextWrapped("SYMPTOM EXPLANATION: %s",
                           headline_analysis.diagnosis.incident_type.c_str());
        ImGui::TextWrapped("LIKELY CONTRIBUTOR: %s",
            headline_analysis.diagnosis.primary_contributor.empty()
                ? "No process cleared the evidence threshold"
                : headline_analysis.diagnosis.primary_contributor.c_str());
        ImGui::Text("UNCERTAINTY: %s (%.0f%% calibrated confidence; %.0f%% evidence coverage)",
                    headline_analysis.diagnosis.confidence.c_str(),
                    headline_analysis.diagnosis.calibrated_confidence * 100.0,
                    headline_analysis.diagnosis.evidence_coverage * 100.0);
        if (!headline_analysis.diagnosis.evidence.empty()) {
            ImGui::TextWrapped("PLAIN EVIDENCE: %s",
                                headline_analysis.diagnosis.evidence.front().c_str());
        }
        ImGui::TextDisabled(
            "Basis: %s. Pressure and contributors remain correlation, not proof of cause.",
            headline_analysis.diagnosis.basis.c_str());
    } else {
        ImGui::TextWrapped(
            headline_analysis.diagnosis.suppressed_by_feedback
                ? "SYMPTOM EXPLANATION: Unknown - repeated matching triggers were not noticed"
                : "SYMPTOM EXPLANATION: Unknown - no independent alignment");
        ImGui::TextWrapped("LIKELY CONTRIBUTOR: Not enough evidence to rank reliably");
        ImGui::TextWrapped("UNCERTAINTY: High - inspect the timelines and availability notes below");
    }
    ImGui::Text("Captured %s | requested %.2f to %.2f s | actual %.2f to %.2f s | triggers %u",
                detail.created_utc.c_str(), detail.requested_start_seconds,
                detail.requested_end_seconds, detail.actual_start_seconds,
                detail.actual_end_seconds, detail.trigger_count);
    ImGui::SetNextItemWidth(260.0F);
    ImGui::InputText("Label", state.label_editor.data(), state.label_editor.size());
    ImGui::InputTextMultiline("Note", state.note_editor.data(), state.note_editor.size(),
                              ImVec2{-1.0F, 70.0F});
    constexpr const char* category_names[] = {
        "Unknown", "System freeze", "Game stutter",
        "Application slowdown/hang", "Network", "Audio"};
    auto category_index = static_cast<int>(state.category_editor);
    ImGui::SetNextItemWidth(260.0F);
    if (ImGui::Combo("Incident category", &category_index, category_names,
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
    ImGui::SetNextItemWidth(260.0F);
    ImGui::InputTextWithHint(
        "Recurring group override", "Optional user group name",
        state.recurring_group_override_editor.data(),
        state.recurring_group_override_editor.size());
    if (ImGui::Button("Save recurring override")) {
        command.action = DashboardAction::save_recurring_group_override;
        command.incident_id = detail.id;
        command.recurring_group_override =
            state.recurring_group_override_editor.data();
    }
    ImGui::SameLine();
    if (ImGui::Button("Return to automatic grouping")) {
        state.recurring_group_override_editor.fill('\0');
        command.action = DashboardAction::save_recurring_group_override;
        command.incident_id = detail.id;
        command.recurring_group_override.clear();
    }
    ImGui::TextDisabled(
        "Matching non-empty names force incidents into one user-overridden group.");

    if (detail.automatic_trigger_count != 0U) {
        ImGui::TextColored(
            ImVec4{1.0F, 0.75F, 0.25F, 1.0F},
            "Automatically captured for %s: %s (%u automatic, %u manual trigger%s)",
            automatic_resource_text(detail.automatic_resource),
            automatic_signal_text(detail.automatic_signal),
            detail.automatic_trigger_count, detail.manual_trigger_count,
            detail.manual_trigger_count == 1U ? "" : "s");
        ImGui::TextDisabled("Observed %.3g | baseline %.3g | detector score %.2f",
                            detail.automatic_observed_value,
                            detail.automatic_baseline_value,
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
                    ? "problem noticed" : "no problem noticed");
        }
    }

    ImGui::SeparatorText("Potential contributors");
    const auto& analysis = detail.analysis;
    if (analysis.state == IncidentAnalysisViewState::disabled) {
        ImGui::TextDisabled("Analysis disabled; recording and viewing are unchanged.");
    } else if (analysis.state == IncidentAnalysisViewState::error) {
        ImGui::TextColored(ImVec4{1.0F, 0.45F, 0.35F, 1.0F}, "%s",
                           analysis.status.c_str());
    } else {
        ImGui::TextWrapped("%s", analysis.status.c_str());
        ImGui::SeparatorText("Observed resource pressure");
        if (analysis.pressure.available) {
            ImGui::Text("%s via %s - %.1f%% practical pressure score",
                        analysis.pressure.resource.c_str(),
                        analysis.pressure.metric.c_str(),
                        analysis.pressure.score * 100.0);
            ImGui::TextWrapped("%s", analysis.pressure.evidence.c_str());
            ImGui::TextDisabled(
                "This pressure was measured near the marker. It may be unrelated to the reported symptom.");
        } else {
            ImGui::TextDisabled(
                "No resource metric cleared its practical-effect floor. Raw statistical evidence remains below.");
        }
        ImGui::SeparatorText("Symptom explanation");
        if (analysis.diagnosis.pipeline_version == 0U) {
            ImGui::TextDisabled("This analyzer does not emit the versioned explanation model.");
        } else if (!analysis.diagnosis.available) {
            ImGui::TextDisabled(
                analysis.diagnosis.suppressed_by_feedback
                    ? "Unknown: bounded local feedback suppressed this automatic-trigger assertion. Raw evidence remains inspectable below."
                    : "Unknown: observed pressure did not align with an independent symptom signal. Evidence remains inspectable below.");
            ImGui::TextDisabled(
                "Pipeline v%u / evidence v%u / configuration %016llx | %s",
                analysis.diagnosis.pipeline_version,
                analysis.diagnosis.evidence_model_version,
                static_cast<unsigned long long>(
                    analysis.diagnosis.configuration_fingerprint),
                analysis.diagnosis.inference.c_str());
        } else {
            ImGui::Text("%s - %s (%.1f%% calibrated)",
                        analysis.diagnosis.incident_type.c_str(),
                        analysis.diagnosis.confidence.c_str(),
                        analysis.diagnosis.calibrated_confidence * 100.0);
            ImGui::TextWrapped("Explanation basis: %s",
                               analysis.diagnosis.basis.c_str());
            ImGui::TextWrapped(
                "This is a probabilistic local symptom explanation, not proof of cause. Every factor below "
                "references recorded evidence; correlated factors are explicitly penalized.");
            if (!analysis.diagnosis.primary_contributor.empty()) {
                ImGui::TextWrapped(
                    "Highest aligned preceding contributor: %s (correlation only)",
                    analysis.diagnosis.primary_contributor.c_str());
            }
            ImGui::TextDisabled(
                "Evidence coverage %.0f%% | correlated-evidence penalty %.1f%% | "
                "pipeline v%u / evidence v%u / configuration %016llx | %s",
                analysis.diagnosis.evidence_coverage * 100.0,
                analysis.diagnosis.correlated_evidence_penalty * 100.0,
                analysis.diagnosis.pipeline_version,
                analysis.diagnosis.evidence_model_version,
                static_cast<unsigned long long>(
                    analysis.diagnosis.configuration_fingerprint),
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
                render_disabled_wrapped(
                    "Historical context only. It does not change this diagnosis, confidence, contributor ranking, or recorded evidence.");
            } else if (analysis.similar_incidents.manually_excluded) {
                render_disabled_wrapped(
                    "Manual grouping remains available for navigation but cannot teach the analysis pipeline.");
            } else {
                render_disabled_wrapped(
                    "Sparse or conflicting feedback is retained for inspection and is not reused.");
            }
        }
        if (analysis.feedback.applicable || analysis.similar_incidents.applicable) {
            ImGui::SeparatorText("Feedback influence controls");
            ImGui::TextDisabled(
                "Profile revision %llu%s",
                static_cast<unsigned long long>(analysis.feedback.profile_revision),
                analysis.feedback.reset_after_utc_milliseconds > 0
                    ? " | reset baseline active"
                    : "");
            ImGui::Checkbox("Confirm reset of learned feedback influence",
                            &product.feedback_reset_confirmed);
            const bool feedback_reset_disabled =
                !product.feedback_reset_confirmed;
            if (feedback_reset_disabled) ImGui::BeginDisabled();
            if (ImGui::Button("Reset feedback influence")) {
                command.action = DashboardAction::reset_feedback_profile;
                product.feedback_reset_confirmed = false;
            }
            if (feedback_reset_disabled) ImGui::EndDisabled();
            if (analysis.feedback.rollback_available) {
                ImGui::SameLine();
                if (ImGui::Button("Undo last reset")) {
                    command.action =
                        DashboardAction::rollback_feedback_profile_reset;
                    product.feedback_reset_confirmed = false;
                }
            }
            render_disabled_wrapped(
                "Automatic-trigger calibration uses exact resource/signal matches. Similar-incident context uses only bounded prior confirmations from automatic recurrence groups.");
            ImGui::TextWrapped(
                "Resetting stops older feedback from influencing future diagnoses or historical context. "
                "It preserves every incident, annotation, and recorded sample.");
        }
        ImGui::SeparatorText("Probabilistic workload context");
        if (!analysis.context.enabled) {
            ImGui::TextDisabled("Context recognition is disabled; anomaly scores are unadjusted.");
        } else {
            ImGui::Text("%s (%.0f%% probability, %.0f%% uncertainty)",
                        analysis.context.primary.c_str(),
                        analysis.context.confidence * 100.0,
                        analysis.context.uncertainty * 100.0);
            ImGui::TextWrapped(
                "Context is an uncertain local heuristic, not a record of user intent. "
                "It can only softly reduce workload-expected anomaly ranks; raw evidence remains visible.");
            if (ImGui::BeginTable("Context probabilities", 2,
                                  ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_RowBg |
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
        ImGui::TextWrapped(
            "Contributor ranks combine correlation evidence; they do not prove causation. "
            "Marker-spanning activity is ambiguous; post-marker activity is shown as a possible "
            "victim or reaction, not a causal rank.");
        render_disabled_wrapped(
            "Attribution is an explicit causal judgment, separate from whether you noticed a problem. "
            "It affects only future exact executable/resource matches after four consistent incidents. "
            "Positive learning requires that the attributed activity genuinely preceded its marker.");
        if (analysis.contributors.empty()) {
            ImGui::TextDisabled("No process has sufficient anomaly evidence for contributor ranking.");
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
                    std::snprintf(
                        calibration, sizeof(calibration),
                        "Incident-local %.0f%% -> calibrated %.0f%% (x%.3f)",
                        row.score_before_feedback * 100.0,
                        row.score * 100.0, row.feedback_multiplier);
                    render_disabled_wrapped(calibration);
                }
                ImGui::TableSetColumnIndex(4);
                constexpr const char* attribution_names[] = {
                    "Unsure", "Confirmed contributor", "Not a contributor"};
                auto attribution = static_cast<int>(row.attribution);
                ImGui::BeginDisabled(row.executable_key.empty());
                ImGui::SetNextItemWidth(-1.0F);
                if (ImGui::Combo("##ContributorAttribution", &attribution,
                                 attribution_names,
                                 static_cast<int>(std::size(attribution_names)))) {
                    command.action = DashboardAction::save_contributor_feedback;
                    command.incident_id = detail.id;
                    command.contributor_executable_key = row.executable_key;
                    command.contributor_resource = row.resource;
                    command.contributor_attribution =
                        static_cast<IncidentContributorRow::Attribution>(attribution);
                    command.contributor_temporal_relationship =
                        row.temporal_relationship;
                }
                ImGui::EndDisabled();
                render_disabled_wrapped(row.feedback_status.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Statistical and personalized anomaly ranking");
        ImGui::TextDisabled(
            "Baseline %.1f to %.1f s | evaluation %.1f to %.1f s | missing values %zu",
            analysis.baseline_start_seconds, analysis.baseline_end_seconds,
            analysis.evaluation_start_seconds, analysis.evaluation_end_seconds,
            analysis.missing_values);
        ImGui::TextDisabled(
            "Scores describe unusual behavior, not proven causation. Confidence reflects baseline coverage.");
        const auto render_ranking = [](const char* table_id,
                                       const std::vector<IncidentAnomalyRow>& rows,
                                       const float height) {
            if (ImGui::BeginTable(table_id, 4,
                                  ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_ScrollY,
                                  ImVec2{-1.0F, height})) {
                ImGui::TableSetupColumn("Candidate", ImGuiTableColumnFlags_WidthFixed, 180.0F);
                ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 75.0F);
                ImGui::TableSetupColumn("Confidence", ImGuiTableColumnFlags_WidthFixed, 125.0F);
                ImGui::TableSetupColumn("Strongest evidence",
                                        ImGuiTableColumnFlags_WidthStretch, 1.0F);
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
            ImGui::TextDisabled("Top process candidates (personalized when history is ready; bounded to %zu)",
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
    ImGui::TextDisabled("Pan, wheel-zoom, or hover any timeline; zoom and cursor stay synchronized.");
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

    ImGui::TextWrapped(
        "Physical-disk quality is measured below the process I/O layer. A process doing I/O may correlate with a stall, but its byte count does not prove it caused device latency or queueing.");
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
    render_missing_summary("Physical-disk latency",
                           detail.disk_service_time_milliseconds);
    render_missing_summary("Physical-disk queue", detail.disk_queue_depth);

    ImGui::TextWrapped(
        "Network quality is passive, host-wide evidence. Windows connectivity state, interface transitions, TCP retransmissions, failed opens, and resets do not identify an application, remote endpoint, physical packet loss, DNS failure, or payload cause. BlackBox sends no probe traffic and does not claim RTT latency.");
    if (ImPlot::BeginPlot("Network connectivity and transport quality",
                          ImVec2{-1.0F, 210.0F})) {
        ImPlot::SetupAxes("Seconds from event", "Level / percent / events");
        ImPlot::SetupAxisLinks(ImAxis_X1, &product.timeline_min, &product.timeline_max);
        render_series("Connectivity (0 none, 1 local, 2 internet, 3 constrained, 4 unknown)",
                      detail.network_connectivity_level);
        render_series("TCP retransmit %", detail.network_tcp_retransmit_percent);
        render_series("Interface transitions", detail.network_interface_changes);
        render_series("Failed TCP connections",
                      detail.network_tcp_failed_connections);
        render_series("Reset TCP connections", detail.network_tcp_resets);
        render_timeline_references(product, x_min, x_max);
        ImPlot::EndPlot();
    }
    render_missing_summary("Windows connectivity",
                           detail.network_connectivity_level);
    render_missing_summary("TCP retransmission",
                           detail.network_tcp_retransmit_percent);

    ImGui::TextWrapped(
        "GPU utilization is the busiest physical engine, not a sum across engines. A foreground GPU drop may describe a victim waiting for work; foreground identity and timing are correlations, never causal proof.");
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

    ImGui::TextWrapped(
        "DPC and interrupt time can starve time-sensitive audio or rendering threads, but aggregate activity cannot identify a driver. The CPU limit is Windows' reported thermal ceiling; a low ceiling is evidence of throttling, not a temperature or root-cause claim.");
    if (ImPlot::BeginPlot("Responsiveness and power evidence",
                          ImVec2{-1.0F, 210.0F})) {
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

    ImGui::SeparatorText("Foreground and Windows event evidence");
    ImGui::TextDisabled(
        "The recorder never stores window titles, Event Log messages/payloads, device/audio IDs, or storage addresses.");
    if (!detail.foreground_applications.empty() &&
        ImGui::BeginTable("Foreground transitions", 3,
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 75.0F);
        ImGui::TableSetupColumn("Foreground application");
        ImGui::TableSetupColumn("GPU", ImGuiTableColumnFlags_WidthFixed, 80.0F);
        ImGui::TableHeadersRow();
        for (const auto& row : detail.foreground_applications) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%+.2f s", row.seconds_from_event);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s (PID %u)", row.name.c_str(), row.identity.pid);
            ImGui::TableSetColumnIndex(2);
            row.gpu_available ? ImGui::Text("%.1f%%", row.gpu_percent)
                              : ImGui::TextDisabled("N/A");
        }
        ImGui::EndTable();
    }
    if (detail.system_events.empty()) {
        ImGui::TextDisabled("No enabled Windows event source delivered an event in this window.");
    } else if (ImGui::BeginTable("System event evidence", 4,
                                 ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollY,
                                 ImVec2{-1.0F, 180.0F})) {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 75.0F);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 150.0F);
        ImGui::TableSetupColumn("Event");
        ImGui::TableSetupColumn("Level / ID", ImGuiTableColumnFlags_WidthFixed, 105.0F);
        ImGui::TableHeadersRow();
        for (const auto& row : detail.system_events) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%+.2f s", row.seconds_from_event);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(row.source.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(row.event.c_str());
            ImGui::TableSetColumnIndex(3); ImGui::Text("%s / %u", row.level.c_str(), row.native_event_id);
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Incident processes");
    auto process_view_changed = ImGui::InputTextWithHint(
        "##incident-process-filter", "Filter name, path, or PID",
        state.process_filter.data(), state.process_filter.size());
    ImGui::SameLine();
    constexpr const char* process_sort_names[]{"Name", "PID", "Peak CPU", "Peak memory",
                                                "Peak read", "Peak write"};
    auto process_sort_index = static_cast<int>(state.process_sort);
    ImGui::SetNextItemWidth(130.0F);
    process_view_changed = ImGui::Combo(
        "##incident-process-sort", &process_sort_index, process_sort_names,
        static_cast<int>(std::size(process_sort_names))) || process_view_changed;
    state.process_sort = static_cast<IncidentProcessSort>(process_sort_index);
    ImGui::SameLine();
    process_view_changed = ImGui::Checkbox("Ascending", &state.process_sort_ascending) ||
                           process_view_changed;
    if (process_view_changed || state.visible_process_indices.empty()) {
        state.visible_process_indices = filter_and_sort_processes(
            detail.processes, state.process_filter.data(), state.process_sort,
            state.process_sort_ascending);
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
            const auto id = process.name + "##process-" +
                            std::to_string(process.identity.pid) + "-" +
                            std::to_string(process.identity.creation_token);
            const auto selected = detail.selected_process &&
                                  *detail.selected_process == process.identity;
            if (ImGui::Selectable(id.c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
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
        const auto title = "Selected process timeline (PID " +
                           std::to_string(detail.selected_process->pid) + ")";
        if (ImPlot::BeginPlot(title.c_str(), ImVec2{-1.0F, 200.0F})) {
            ImPlot::SetupAxes("Seconds from event", "CPU % / MiB / MiB/s");
            ImPlot::SetupAxisLinks(ImAxis_X1, &product.timeline_min, &product.timeline_max);
            render_series("CPU %", detail.selected_process_cpu_percent);
            render_series("Working set MiB", detail.selected_process_working_set_mib);
            render_series("Disk read MiB/s",
                          detail.selected_process_disk_read_mib_per_second);
            render_series("Disk write MiB/s",
                          detail.selected_process_disk_write_mib_per_second);
            render_timeline_references(product, x_min, x_max);
            ImPlot::EndPlot();
        }
    }
}

void render_product_settings(const DashboardState& state, ProductUiState& product,
                             DashboardCommand& command) {
    ImGui::SeparatorText("Capture and privacy settings");
    ImGui::TextWrapped(
        "Settings are validated before they cross into the recorder. Changing archive path or capacity is saved for the next launch; existing incidents are never moved or deleted automatically.");

    constexpr const char* keys[]{"F1", "F2", "F3", "F4", "F5", "F6",
                                  "F7", "F8", "F9", "F10", "F11", "F12"};
    auto key_index = static_cast<int>(std::clamp<std::uint32_t>(product.hotkey_key, 1U, 12U) - 1U);
    ImGui::SetNextItemWidth(100.0F);
    if (ImGui::Combo("Incident hotkey", &key_index, keys, 12)) {
        product.hotkey_key = static_cast<std::uint32_t>(key_index + 1);
    }
    ImGui::Checkbox("Ctrl", &product.hotkey_control); ImGui::SameLine();
    ImGui::Checkbox("Shift", &product.hotkey_shift); ImGui::SameLine();
    ImGui::Checkbox("Alt", &product.hotkey_alt); ImGui::SameLine();
    ImGui::Checkbox("Windows", &product.hotkey_windows);

    ImGui::InputScalar("Seconds before incident", ImGuiDataType_U64,
                       &product.incident_pre_window_seconds);
    ImGui::InputScalar("Seconds after incident", ImGuiDataType_U64,
                       &product.incident_post_window_seconds);
    ImGui::Checkbox("Automatic incident detection", &product.automatic_detection);
    constexpr const char* sensitivity[]{"Conservative", "Balanced", "Sensitive"};
    ImGui::Combo("Detector sensitivity", &product.detector_sensitivity,
                 sensitivity, 3);
    ImGui::Checkbox("CPU", &product.detect_cpu); ImGui::SameLine();
    ImGui::Checkbox("Memory", &product.detect_memory); ImGui::SameLine();
    ImGui::Checkbox("Disk", &product.detect_disk); ImGui::SameLine();
    ImGui::Checkbox("Network", &product.detect_network);
    ImGui::InputScalar("Detector cooldown (seconds)", ImGuiDataType_U64,
                       &product.detector_cooldown_seconds);
    ImGui::Checkbox("Desktop notifications", &product.notifications);
    ImGui::Checkbox("Record executable paths in future samples",
                    &product.collect_process_paths);
    ImGui::Checkbox("Record foreground application identity",
                    &product.record_foreground_application);
    ImGui::Checkbox("Record process start and exit identity",
                    &product.record_process_lifecycle);
    ImGui::Checkbox("Record power and device transition events",
                    &product.record_power_and_device_events);
    ImGui::Checkbox("Record audio endpoint transition events",
                    &product.record_audio_device_events);
    ImGui::Checkbox("Record selected privacy-bounded Windows events",
                    &product.record_windows_event_log_evidence);
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextDisabled(
        "Process lifecycle evidence stores a durable PID/creation identity in the local archive when explicitly enabled; exported evaluation datasets redact it. Other event evidence stores only source, event ID, level, and action. Window titles, Event Log messages, device/endpoint IDs, storage addresses, and payloads are never retained. Existing immutable incidents are unchanged until explicitly purged.");
    ImGui::PopTextWrapPos();
    ImGui::InputText("Archive path (restart required)", product.archive_path.data(),
                     product.archive_path.size());
    ImGui::InputScalar("Archive capacity (MiB, restart required)", ImGuiDataType_U64,
                       &product.archive_maximum_mib);
    if (ImGui::Button("Validate, apply, and save settings")) {
        command.action = DashboardAction::apply_product_settings;
        command.hotkey_key = product.hotkey_key;
        command.hotkey_control = product.hotkey_control;
        command.hotkey_shift = product.hotkey_shift;
        command.hotkey_alt = product.hotkey_alt;
        command.hotkey_windows = product.hotkey_windows;
        command.automatic_detection = product.automatic_detection;
        command.detector_sensitivity = product.detector_sensitivity;
        command.detect_cpu = product.detect_cpu;
        command.detect_memory = product.detect_memory;
        command.detect_disk = product.detect_disk;
        command.detect_network = product.detect_network;
        command.detector_cooldown_seconds = product.detector_cooldown_seconds;
        command.notifications = product.notifications;
        command.collect_process_paths = product.collect_process_paths;
        command.record_foreground_application = product.record_foreground_application;
        command.record_process_lifecycle = product.record_process_lifecycle;
        command.record_power_and_device_events = product.record_power_and_device_events;
        command.record_audio_device_events = product.record_audio_device_events;
        command.record_windows_event_log_evidence =
            product.record_windows_event_log_evidence;
        command.incident_pre_window_seconds = product.incident_pre_window_seconds;
        command.incident_post_window_seconds = product.incident_post_window_seconds;
        command.archive_maximum_mib = product.archive_maximum_mib;
        command.archive_path = product.archive_path.data();
    }
    ImGui::TextDisabled("%s", state.recorder_settings_status.data());

    ImGui::SeparatorText("Archive health and guided recovery");
    const auto used_mib = static_cast<double>(state.archive_database_size_bytes) /
                          (1024.0 * 1024.0);
    const auto maximum_mib = static_cast<double>(state.archive_maximum_bytes) /
                             (1024.0 * 1024.0);
    ImGui::Text("%s | schema %d | %llu incidents | %.1f / %.1f MiB",
                state.archive_healthy ? "Healthy" : "Needs attention",
                state.archive_schema_version,
                static_cast<unsigned long long>(state.stored_incident_count),
                used_mib, maximum_mib);
    ImGui::TextWrapped("Archive: %s", state.archive_path.data());
    ImGui::TextWrapped("%s", state.archive_maintenance_status.data());
    if (state.archive_maintenance_busy) ImGui::BeginDisabled();
    if (ImGui::Button("Refresh archive health")) {
        command.action = DashboardAction::refresh_archive_health;
    }
    if (state.archive_recoverable_incident) {
        ImGui::SameLine();
        if (ImGui::Button("Retry failed incident")) {
            command.action = DashboardAction::retry_failed_incident;
        }
        ImGui::TextColored(ImVec4{1.0F, 0.75F, 0.25F, 1.0F},
                           "Capture %llu is preserved in the bounded recovery slot.",
                           static_cast<unsigned long long>(state.archive_recoverable_sequence));
    }

    ImGui::InputText("New backup file", product.backup_path.data(),
                     product.backup_path.size());
    if (ImGui::Button("Create verified backup")) {
        command.action = DashboardAction::backup_archive;
        command.backup_path = product.backup_path.data();
    }
    ImGui::InputText("Restore source", product.restore_path.data(),
                     product.restore_path.size());
    ImGui::InputText("New pre-restore safety backup", product.safety_backup_path.data(),
                     product.safety_backup_path.size());
    ImGui::Checkbox("I understand restore replaces the active archive",
                    &product.restore_confirmed);
    const auto restore_disabled = !product.restore_confirmed;
    if (restore_disabled) ImGui::BeginDisabled();
    if (ImGui::Button("Validate source and restore")) {
        command.action = DashboardAction::restore_archive;
        command.restore_path = product.restore_path.data();
        command.safety_backup_path = product.safety_backup_path.data();
        product.restore_confirmed = false;
    }
    if (restore_disabled) ImGui::EndDisabled();

    ImGui::InputText("Dataset export directory", product.export_path.data(),
                     product.export_path.size());
    if (ImGui::Button("Export inspectable evidence dataset")) {
        command.action = DashboardAction::export_dataset;
        command.export_path = product.export_path.data();
    }
    if (state.archive_recoverable_incident) {
        ImGui::InputText("Failed incident export file", product.failed_export_path.data(),
                         product.failed_export_path.size());
        if (ImGui::Button("Export failed incident before retry")) {
            command.action = DashboardAction::export_failed_incident;
            command.failed_export_path = product.failed_export_path.data();
        }
    }

    ImGui::InputScalar("Keep newest incidents", ImGuiDataType_U64,
                       &product.retention_incidents);
    ImGui::Checkbox("Confirm explicit retention deletion", &product.retention_confirmed);
    const auto retention_disabled = !product.retention_confirmed;
    if (retention_disabled) ImGui::BeginDisabled();
    if (ImGui::Button("Apply retention now")) {
        command.action = DashboardAction::retain_incidents;
        command.retention_incidents = static_cast<std::size_t>(product.retention_incidents);
        product.retention_confirmed = false;
    }
    if (retention_disabled) ImGui::EndDisabled();

    ImGui::Checkbox("Confirm permanent privacy purge of incidents and profiles",
                    &product.purge_confirmed);
    const auto purge_disabled = !product.purge_confirmed;
    if (purge_disabled) ImGui::BeginDisabled();
    if (ImGui::Button("Purge all local incident evidence")) {
        command.action = DashboardAction::purge_archive;
        product.purge_confirmed = false;
    }
    if (purge_disabled) ImGui::EndDisabled();
    if (state.archive_maintenance_busy) ImGui::EndDisabled();
}

} // namespace

bool set_timeline_cursor(ProductUiState& product,
                         const double seconds_from_event,
                         const double incident_start_seconds,
                         const double incident_end_seconds) noexcept {
    if (!std::isfinite(seconds_from_event) ||
        !std::isfinite(incident_start_seconds) ||
        !std::isfinite(incident_end_seconds) ||
        incident_start_seconds > incident_end_seconds) {
        return false;
    }
    product.timeline_cursor_seconds = std::clamp(
        seconds_from_event, incident_start_seconds, incident_end_seconds);
    product.timeline_cursor_visible = true;
    return true;
}

void clear_timeline_cursor(ProductUiState& product) noexcept {
    product.timeline_cursor_visible = false;
    product.timeline_cursor_seconds = 0.0;
}

DashboardCommand render_dashboard(const DashboardState& state,
                                  IncidentViewerState& incident_viewer,
                                  ProductUiState& product) {
    DashboardCommand command{};

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                       ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("BlackBox Dashboard", nullptr, flags)) {
        ImGui::TextUnformatted("BlackBox");
        ImGui::SameLine();
        ImGui::TextDisabled("Computer Flight Recorder - local evidence, honest uncertainty");
        ImGui::Separator();
        ImGui::Spacing();

        constexpr const char* page_names[]{"Live", "Incidents", "Detail", "Patterns",
                                            "Settings", "Diagnostics"};
        for (std::size_t index = 0U; index < std::size(page_names); ++index) {
            if (index != 0U) ImGui::SameLine();
            const auto selected = product.page == static_cast<ProductPage>(index);
            if (selected) ImGui::BeginDisabled();
            if (ImGui::Button(page_names[index])) {
                product.page = static_cast<ProductPage>(index);
            }
            if (selected) ImGui::EndDisabled();
            const auto key = static_cast<ImGuiKey>(ImGuiKey_1 + static_cast<int>(index));
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(key, false)) {
                product.page = static_cast<ProductPage>(index);
            }
        }
        ImGui::Separator();

        if (product.onboarding_open) ImGui::OpenPopup("Welcome to BlackBox");
        if (ImGui::BeginPopupModal("Welcome to BlackBox", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(
                "BlackBox keeps a bounded rolling history in memory. Press the configured hotkey when a slowdown happens; only then is an immutable before/after incident saved locally.");
            ImGui::TextWrapped(
                "Automatic detection is conservative and configurable. Diagnoses describe likely contributors and uncertainty from recorded correlation - never proof of cause.");
            ImGui::TextWrapped(
                "Unavailable means Windows could not provide a metric. Warming up means a counter needs an earlier observation. Neither state is silently treated as zero.");
            if (ImGui::Button("Start recording")) {
                product.onboarding_open = false;
                command.action = DashboardAction::complete_onboarding;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (product.page == ProductPage::live) {
        if (ImGui::BeginTable("Status", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
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
            row("Accessibility", state.accessibility_high_contrast
                                     ? "Windows high contrast" : "Standard contrast");
            row("System animations", state.accessibility_animations_enabled
                                     ? "Enabled" : "Disabled by Windows");
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("Display scale");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.0f%% (PerMonitorV2)", state.display_scale * 100.0);
            row("Version", core::version.data());
            ImGui::EndTable();
        }
        ImGui::TextWrapped(
            "Unavailable metrics retain their reason (unsupported, inaccessible, or temporary). Cold-start values require a previous counter observation. Analysis is post-capture and correlation never establishes causation.");
        }

        if (product.page == ProductPage::live) {
        ImGui::Spacing();
        ImGui::SeparatorText("Manual incident capture");
        if (!state.incident_capture_enabled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Capture incident (configured hotkey)")) {
            command.action = DashboardAction::capture_incident;
        }
        if (!state.incident_capture_enabled) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(state.incident_capture_status.data());
        ImGui::Text("Window: %.0f s before / %.0f s after",
                    state.incident_pre_window_seconds,
                    state.incident_post_window_seconds);
        if (state.incident_post_remaining_seconds > 0.0) {
            ImGui::Text("Post-window remaining: %.1f s",
                        state.incident_post_remaining_seconds);
        }
        ImGui::Text("Writer handoff: %zu / %zu immutable incidents",
                    state.incident_queue_size, state.incident_queue_capacity);
        ImGui::Text("Automatic detection: %s | %llu triggers | %llu cooldown suppressions | %llu rejected",
                    state.automatic_detection_enabled ? "enabled" : "disabled",
                    static_cast<unsigned long long>(state.automatic_detector_triggers),
                    static_cast<unsigned long long>(
                        state.automatic_detector_cooldown_suppressions),
                    static_cast<unsigned long long>(state.automatic_capture_rejections));
        ImGui::TextWrapped("Windows symptom capture: %s | %llu event request%s",
                           state.automatic_event_capture_status.c_str(),
                           static_cast<unsigned long long>(
                               state.automatic_event_capture_requests),
                           state.automatic_event_capture_requests == 1U ? "" : "s");
        ImGui::TextDisabled("Frame pacing capture: %s",
                            state.automatic_frame_capture_status.c_str());
        ImGui::TextDisabled("Audio glitch capture: %s",
                            state.automatic_audio_capture_status.c_str());
        }

        if (product.page == ProductPage::incidents ||
            product.page == ProductPage::detail ||
            product.page == ProductPage::patterns) {
            render_incident_viewer(incident_viewer, command, product);
        }

        if (product.page == ProductPage::settings) {
        ImGui::Spacing();
        ImGui::SeparatorText("Recorder settings");
        ImGui::TextWrapped("Changing profile restarts the collector, clears only rolling RAM "
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
        render_product_settings(state, product, command);
        }

        if (product.page == ProductPage::live) {
        ImGui::Spacing();
        ImGui::SeparatorText("Live system telemetry");

        ImGui::TextUnformatted("Total CPU utilization");
        if (state.cpu_status == MetricDisplayStatus::available) {
            const auto cpu_fraction = static_cast<float>(std::clamp(state.cpu_usage, 0.0, 1.0));
            char overlay[32]{};
            std::snprintf(overlay, sizeof(overlay), "%.1f%%", state.cpu_usage * 100.0);
            ImGui::ProgressBar(cpu_fraction, ImVec2{-1.0F, 0.0F}, overlay);
        } else {
            render_metric_unavailable(state.cpu_status);
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Physical memory utilization");
        if (state.memory_status == MetricDisplayStatus::available) {
            const auto memory_fraction = static_cast<float>(std::clamp(state.memory_usage, 0.0, 1.0));
            const auto used_gib = static_cast<double>(state.memory_used_bytes) / (1024.0 * 1024.0 * 1024.0);
            const auto total_gib = static_cast<double>(state.memory_total_bytes) / (1024.0 * 1024.0 * 1024.0);
            char overlay[64]{};
            std::snprintf(overlay, sizeof(overlay), "%.1f / %.1f GiB (%.1f%%)",
                          used_gib, total_gib, state.memory_usage * 100.0);
            ImGui::ProgressBar(memory_fraction, ImVec2{-1.0F, 0.0F}, overlay);
        } else {
            render_metric_unavailable(state.memory_status);
        }

        ImGui::Spacing();
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
        ImGui::TextWrapped(
            "Physical storage quality is separate from process I/O. Network quality is passive host-wide transport/connectivity evidence; no RTT probe or application payload is inspected.");
        if (ImGui::BeginTable("Live forensic quality", 2,
                              ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthFixed, 230.0F);
            ImGui::TableSetupColumn("Observation");
            const auto value_row = [](const char* label,
                                      const MetricDisplayStatus status,
                                      const char* format, const double value) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(label);
                ImGui::TableSetColumnIndex(1);
                if (status == MetricDisplayStatus::available) ImGui::Text(format, value);
                else render_metric_unavailable(status);
            };
            value_row("Worst physical-disk service time",
                      state.disk_latency_status, "%.2f ms", state.disk_service_time_milliseconds);
            value_row("Worst physical-disk queue", state.disk_queue_status,
                      "%.2f requests", state.disk_queue_depth);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("Windows connectivity");
            ImGui::TableSetColumnIndex(1);
            if (state.network_connectivity_status == MetricDisplayStatus::available) {
                ImGui::Text("%s | %llu active interface%s | %llu transition%s",
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
            value_row("DPC processor time", state.dpc_status, "%.2f%%",
                      state.dpc_usage * 100.0);
            value_row("Interrupt processor time", state.dpc_status, "%.2f%%",
                      state.interrupt_usage * 100.0);
            value_row("Average CPU frequency", state.cpu_frequency_status, "%.0f MHz",
                      state.cpu_current_mhz);
            value_row("CPU thermal ceiling", state.cpu_thermal_limit_status, "%.1f%%",
                      state.cpu_thermal_limit_fraction * 100.0);
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
        ImGui::TextDisabled(
            "Windows event recorder: %s | %zu / %zu events | %llu recorded | %llu dropped/overwritten",
            state.event_collector_running ? "running" : "disabled/stopped",
            state.system_event_ring_size, state.system_event_ring_capacity,
            static_cast<unsigned long long>(state.system_events_recorded),
            static_cast<unsigned long long>(state.system_events_dropped));
        ImGui::TextDisabled(
            "Process lifecycle: %llu provider observations | %llu opt-in events recorded",
            static_cast<unsigned long long>(state.process_lifecycle_observations),
            static_cast<unsigned long long>(
                state.process_lifecycle_events_recorded));
        }

        if (product.page == ProductPage::live) {
        ImGui::Spacing();
        ImGui::SeparatorText("Rolling five-minute history");
        if (state.history_size == 0U) {
            ImGui::TextDisabled("Waiting for recorder samples");
        } else if (ImPlot::BeginPlot("##system-history", ImVec2{-1.0F, 220.0F},
                                     ImPlotFlags_NoMouseText)) {
            ImPlot::SetupAxes("Oldest to newest sample", "Utilization (%)",
                              ImPlotAxisFlags_NoTickLabels,
                              ImPlotAxisFlags_None);
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

        const auto newest_sample = state.history_size > 1U
                                       ? static_cast<double>(state.history_size - 1U)
                                       : 1.0;
        if (state.disk_read_history_points != 0U ||
            state.disk_write_history_points != 0U) {
            if (ImPlot::BeginPlot("Disk throughput", ImVec2{-1.0F, 180.0F},
                                  ImPlotFlags_NoMouseText)) {
                ImPlot::SetupAxes("Oldest to newest sample", "MiB/s",
                                  ImPlotAxisFlags_NoTickLabels,
                                  ImPlotAxisFlags_None);
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
                                  ImPlotAxisFlags_NoTickLabels,
                                  ImPlotAxisFlags_None);
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

        ImGui::Spacing();
        ImGui::SeparatorText("Active processes (highest CPU first)");
        if (state.process_count == 0U) {
            ImGui::TextDisabled("Process telemetry is warming up");
        } else if (ImGui::BeginTable(
                       "Active processes", 6,
                       ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                       ImVec2{-1.0F, 260.0F})) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Process");
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 70.0F);
            ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthFixed, 80.0F);
            ImGui::TableSetupColumn("Working set", ImGuiTableColumnFlags_WidthFixed, 100.0F);
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

        if (product.page == ProductPage::diagnostics) {
        ImGui::Spacing();
        ImGui::SeparatorText("Support and crash recovery");
        ImGui::TextWrapped("%s", state.crash_diagnostics_status.data());
        ImGui::Text("Completed local crash dumps: %llu | handler: %s",
                    static_cast<unsigned long long>(state.previous_crash_dumps),
                    state.crash_diagnostics_armed ? "armed" : "not armed");
        ImGui::TextWrapped("%s", state.support_bundle_status.data());
        ImGui::TextDisabled(
            "Bundles stay local and exclude incidents, process rows, settings, hotkeys, usernames, and absolute paths.");
        ImGui::InputText("New support bundle directory",
                         product.support_bundle_path.data(),
                         product.support_bundle_path.size());
        if (!state.latest_crash_dump_available) {
            product.include_latest_crash_dump = false;
            product.crash_dump_consent_confirmed = false;
            ImGui::BeginDisabled();
        }
        ImGui::Checkbox("Include latest crash dump",
                        &product.include_latest_crash_dump);
        if (!state.latest_crash_dump_available) ImGui::EndDisabled();
        if (product.include_latest_crash_dump) {
            ImGui::TextColored(
                ImVec4{1.0F, 0.75F, 0.25F, 1.0F},
                "A minidump can contain stack memory and module paths.");
            ImGui::Checkbox("I consent to place the latest raw dump in this local bundle",
                            &product.crash_dump_consent_confirmed);
        } else {
            product.crash_dump_consent_confirmed = false;
        }
        const bool support_disabled = state.support_bundle_busy ||
            (product.include_latest_crash_dump &&
             !product.crash_dump_consent_confirmed);
        if (support_disabled) ImGui::BeginDisabled();
        if (ImGui::Button("Create local support bundle")) {
            command.action = DashboardAction::create_support_bundle;
            command.support_bundle_path = product.support_bundle_path.data();
            command.include_latest_crash_dump =
                product.include_latest_crash_dump;
            command.crash_dump_disclosure_confirmed =
                product.crash_dump_consent_confirmed;
            product.crash_dump_consent_confirmed = false;
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
            count_row("Memory graph points", static_cast<std::uint64_t>(state.memory_history_points));
            count_row("Disk read graph points", static_cast<std::uint64_t>(state.disk_read_history_points));
            count_row("Network RX graph points", static_cast<std::uint64_t>(state.network_receive_history_points));
            count_row("Active processes", static_cast<std::uint64_t>(state.active_process_count));
            count_row("Process metadata", static_cast<std::uint64_t>(state.process_metadata_count));
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
            count_row("Partial samples", state.partial_samples);
            count_row("Failed samples", state.failed_samples);
            count_row("Dropped ticks", state.dropped_samples);
            count_row("Late starts", state.late_samples);
            count_row("Deadline misses", state.deadline_misses);
            count_row("Resume events", state.resume_events);
            count_row("Resume skipped ticks", state.resume_skipped_samples);
            count_row("Provider recoveries", state.provider_recoveries);
            count_row("Consecutive provider failures",
                      state.consecutive_provider_failures);
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
