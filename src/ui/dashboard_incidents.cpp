#include "ui/dashboard_incidents.hpp"

#include "ui/dashboard.hpp"
#include "ui/product_ui_model.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <implot.h>
#include <string>

namespace blackbox::ui {
namespace {

void render_disabled_wrapped(const char* const text) {
    ImGui::BeginDisabled();
    ImGui::TextWrapped("%s", text);
    ImGui::EndDisabled();
}

void render_compact_decimal(const double value, const char* ordinary_format) {
    if (std::abs(value) >= 0.01) {
        ImGui::Text(ordinary_format, value);
        return;
    }
    ImGui::Text("%.2e", value);
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

void render_incident_viewer_impl(IncidentViewerState& state, DashboardCommand& command,
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

namespace detail {

void render_incident_viewer(IncidentViewerState& state, DashboardCommand& command,
                            ProductUiState& product) {
    render_incident_viewer_impl(state, command, product);
}

} // namespace detail
} // namespace blackbox::ui
