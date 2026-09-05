#include "ui/dashboard_settings.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdint>

namespace blackbox::ui::detail {

std::array<char, product_path_capacity + 1U>&
path_buffer(ProductUiState& product, const PathField field) noexcept {
    switch (field) {
    case PathField::archive: return product.archive_path;
    case PathField::backup: return product.backup_path;
    case PathField::restore: return product.restore_path;
    case PathField::safety_backup: return product.safety_backup_path;
    case PathField::dataset: return product.export_path;
    case PathField::failed_export: return product.failed_export_path;
    case PathField::support_bundle: return product.support_bundle_path;
    case PathField::summary: return product.summary_path;
    }
    return product.archive_path;
}

void render_path_input(const char* label,
                       std::array<char, product_path_capacity + 1U>& path,
                       const PathField field, const ProductUiState& product,
                       DashboardCommand& command) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(std::max(100.0F, ImGui::GetContentRegionAvail().x - 100.0F));
    ImGui::InputText("##path", path.data(), path.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(product.file_dialog_pending);
    if (ImGui::Button("Browse...")) {
        command.action = DashboardAction::browse_path;
        command.path_field = field;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

void render_product_settings(const DashboardState& state, ProductUiState& product,
                             DashboardCommand& command) {
    ImGui::TextWrapped("%s", product.settings_dirty ? "Unsaved changes: apply and save to keep your settings." : "Settings match your saved preferences.");
    if (ImGui::Button("Validate, apply, and save settings")) {
        command.action = DashboardAction::apply_product_settings;
        command.hotkey_key = product.hotkey_key;
        command.hotkey_control = product.hotkey_control;
        command.hotkey_shift = product.hotkey_shift;
        command.hotkey_alt = product.hotkey_alt;
        command.hotkey_system_modifier = product.hotkey_system_modifier;
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
        command.record_system_event_evidence = product.record_system_event_evidence;
        command.incident_pre_window_seconds = product.incident_pre_window_seconds;
        command.incident_post_window_seconds = product.incident_post_window_seconds;
        command.archive_maximum_mib = product.archive_maximum_mib;
        command.archive_path = product.archive_path.data();
    }
    ImGui::TextDisabled("%s", state.recorder_settings_status.data());
    if (!product.file_dialog_status.empty()) ImGui::TextWrapped("%s", product.file_dialog_status.c_str());
    ImGui::SeparatorText("Capture and privacy");
    ImGui::TextWrapped("Choose how BlackBox captures a problem and which optional context may be "
                       "saved with future incidents. Existing incidents never change here.");

    ImGui::SeparatorText("Capture shortcut and time window");
    constexpr const char* keys[]{"F1", "F2", "F3", "F4",  "F5",  "F6",
                                 "F7", "F8", "F9", "F10", "F11", "F12"};
    auto key_index = static_cast<int>(std::clamp<std::uint32_t>(product.hotkey_key, 1U, 12U) - 1U);
    ImGui::SetNextItemWidth(100.0F);
    if (ImGui::Combo("Incident hotkey", &key_index, keys, 12)) {
        product.hotkey_key = static_cast<std::uint32_t>(key_index + 1);
    }
    ImGui::Checkbox("Ctrl", &product.hotkey_control);
    ImGui::SameLine();
    ImGui::Checkbox("Shift", &product.hotkey_shift);
    ImGui::SameLine();
    ImGui::Checkbox("Alt", &product.hotkey_alt);
    ImGui::SameLine();
    ImGui::Checkbox(state.system_modifier_label.c_str(), &product.hotkey_system_modifier);

    ImGui::InputScalar("Seconds before incident", ImGuiDataType_U64,
                       &product.incident_pre_window_seconds);
    ImGui::InputScalar("Seconds after incident", ImGuiDataType_U64,
                       &product.incident_post_window_seconds);

    ImGui::SeparatorText("Automatic capture");
    ImGui::TextDisabled("BlackBox can preserve unusual resource activity even when you do not "
                        "press the shortcut.");
    ImGui::Checkbox("Automatic incident detection", &product.automatic_detection);
    constexpr const char* sensitivity[]{"Conservative", "Balanced", "Sensitive"};
    ImGui::Combo("Detector sensitivity", &product.detector_sensitivity, sensitivity, 3);
    ImGui::Checkbox("CPU", &product.detect_cpu);
    ImGui::SameLine();
    ImGui::Checkbox("Memory", &product.detect_memory);
    ImGui::SameLine();
    ImGui::Checkbox("Disk", &product.detect_disk);
    ImGui::SameLine();
    ImGui::Checkbox("Network", &product.detect_network);
    ImGui::InputScalar("Detector cooldown (seconds)", ImGuiDataType_U64,
                       &product.detector_cooldown_seconds);
    ImGui::SeparatorText("Notifications and optional context");
    ImGui::Checkbox("Show quiet desktop notifications", &product.notifications);
    ImGui::Checkbox("Record executable paths in future samples", &product.collect_process_paths);
    if (!state.foreground_identity_supported) ImGui::BeginDisabled();
    ImGui::Checkbox("Record foreground application identity",
                    &product.record_foreground_application);
    if (!state.foreground_identity_supported) ImGui::EndDisabled();
    ImGui::TextDisabled("%s", state.foreground_identity_support.c_str());
    ImGui::Checkbox("Record process start and exit identity", &product.record_process_lifecycle);
    ImGui::Checkbox("Record power and device transition events",
                    &product.record_power_and_device_events);
    ImGui::Checkbox("Record audio endpoint transition events", &product.record_audio_device_events);
    ImGui::Checkbox("Record selected privacy-bounded system events",
                    &product.record_system_event_evidence);
    if (ImGui::CollapsingHeader("What these privacy options retain")) {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextDisabled("Process lifecycle evidence stores a durable PID/creation identity in "
                            "the local archive when explicitly enabled; exported evaluation "
                            "datasets redact it. Other event evidence stores only source, event "
                            "ID, level, and action. Window titles, Event Log messages, "
                            "device/endpoint IDs, storage addresses, and payloads are never "
                            "retained.");
        ImGui::PopTextWrapPos();
    }
    if (ImGui::CollapsingHeader("Advanced archive location")) {
        ImGui::TextDisabled("Location and capacity changes take effect after restart and never "
                            "move existing incidents automatically.");
        render_path_input("Archive path", product.archive_path, PathField::archive, product, command);
        ImGui::InputScalar("Archive capacity (MiB)", ImGuiDataType_U64,
                           &product.archive_maximum_mib);
    }

    ImGui::SeparatorText("Archive health and guided recovery");
    const auto used_mib =
        static_cast<double>(state.archive_database_size_bytes) / (1024.0 * 1024.0);
    const auto maximum_mib = static_cast<double>(state.archive_maximum_bytes) / (1024.0 * 1024.0);
    ImGui::Text("%s | schema %d | %llu incidents | %.1f / %.1f MiB",
                state.archive_healthy ? "Healthy" : "Needs attention", state.archive_schema_version,
                static_cast<unsigned long long>(state.stored_incident_count), used_mib,
                maximum_mib);
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

    render_path_input("New backup file", product.backup_path, PathField::backup, product, command);
    if (ImGui::Button("Create verified backup")) {
        command.action = DashboardAction::backup_archive;
        command.backup_path = product.backup_path.data();
    }
    render_path_input("Restore source", product.restore_path, PathField::restore, product, command);
    render_path_input("New pre-restore safety backup", product.safety_backup_path, PathField::safety_backup, product, command);
    ImGui::Checkbox("I understand restore replaces the active archive", &product.restore_confirmed);
    const auto restore_disabled = !product.restore_confirmed;
    if (restore_disabled) ImGui::BeginDisabled();
    if (ImGui::Button("Validate source and restore")) {
        command.action = DashboardAction::restore_archive;
        command.restore_path = product.restore_path.data();
        command.safety_backup_path = product.safety_backup_path.data();
        product.restore_confirmed = false;
    }
    if (restore_disabled) ImGui::EndDisabled();

    render_path_input("Dataset export directory", product.export_path, PathField::dataset, product, command);
    if (ImGui::Button("Export inspectable evidence dataset")) {
        command.action = DashboardAction::export_dataset;
        command.export_path = product.export_path.data();
    }
    if (state.archive_recoverable_incident) {
        render_path_input("Failed incident export file", product.failed_export_path, PathField::failed_export, product, command);
        if (ImGui::Button("Export failed incident before retry")) {
            command.action = DashboardAction::export_failed_incident;
            command.failed_export_path = product.failed_export_path.data();
        }
    }

    ImGui::InputScalar("Keep newest incidents", ImGuiDataType_U64, &product.retention_incidents);
    ImGui::Checkbox("Confirm explicit retention deletion", &product.retention_confirmed);
    const auto retention_disabled = !product.retention_confirmed;
    if (retention_disabled) ImGui::BeginDisabled();
    if (ImGui::Button("Apply retention now")) {
        command.action = DashboardAction::retain_incidents;
        command.retention_incidents = static_cast<std::size_t>(product.retention_incidents);
        product.retention_confirmed = false;
    }
    if (retention_disabled) ImGui::EndDisabled();

    ImGui::SeparatorText("Permanent local-data removal");
    ImGui::TextWrapped("Purge deletes the active archive contents, learned profiles, recent in-memory "
                       "history, and the failed-incident recovery slot. Exported files and backups remain.");
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

} // namespace blackbox::ui::detail
