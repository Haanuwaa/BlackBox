#include "ui/dashboard_settings.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdint>

namespace blackbox::ui::detail {

void render_product_settings(const DashboardState &state,
                             ProductUiState &product,
                             DashboardCommand &command) {
  ImGui::SeparatorText("Capture and privacy settings");
  ImGui::TextWrapped(
      "Settings are validated before they cross into the recorder. Changing "
      "archive path or capacity is saved for the next launch; existing "
      "incidents are never moved or deleted automatically.");

  constexpr const char *keys[]{"F1", "F2", "F3", "F4",  "F5",  "F6",
                               "F7", "F8", "F9", "F10", "F11", "F12"};
  auto key_index = static_cast<int>(
      std::clamp<std::uint32_t>(product.hotkey_key, 1U, 12U) - 1U);
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
  ImGui::Checkbox("Windows", &product.hotkey_windows);

  ImGui::InputScalar("Seconds before incident", ImGuiDataType_U64,
                     &product.incident_pre_window_seconds);
  ImGui::InputScalar("Seconds after incident", ImGuiDataType_U64,
                     &product.incident_post_window_seconds);
  ImGui::Checkbox("Automatic incident detection", &product.automatic_detection);
  constexpr const char *sensitivity[]{"Conservative", "Balanced", "Sensitive"};
  ImGui::Combo("Detector sensitivity", &product.detector_sensitivity,
               sensitivity, 3);
  ImGui::Checkbox("CPU", &product.detect_cpu);
  ImGui::SameLine();
  ImGui::Checkbox("Memory", &product.detect_memory);
  ImGui::SameLine();
  ImGui::Checkbox("Disk", &product.detect_disk);
  ImGui::SameLine();
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
      "Process lifecycle evidence stores a durable PID/creation identity in "
      "the local archive when explicitly enabled; exported evaluation datasets "
      "redact it. Other event evidence stores only source, event ID, level, "
      "and action. Window titles, Event Log messages, device/endpoint IDs, "
      "storage addresses, and payloads are never retained. Existing immutable "
      "incidents are unchanged until explicitly purged.");
  ImGui::PopTextWrapPos();
  ImGui::InputText("Archive path (restart required)",
                   product.archive_path.data(), product.archive_path.size());
  ImGui::InputScalar("Archive capacity (MiB, restart required)",
                     ImGuiDataType_U64, &product.archive_maximum_mib);
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
    command.record_foreground_application =
        product.record_foreground_application;
    command.record_process_lifecycle = product.record_process_lifecycle;
    command.record_power_and_device_events =
        product.record_power_and_device_events;
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
  const auto maximum_mib =
      static_cast<double>(state.archive_maximum_bytes) / (1024.0 * 1024.0);
  ImGui::Text("%s | schema %d | %llu incidents | %.1f / %.1f MiB",
              state.archive_healthy ? "Healthy" : "Needs attention",
              state.archive_schema_version,
              static_cast<unsigned long long>(state.stored_incident_count),
              used_mib, maximum_mib);
  ImGui::TextWrapped("Archive: %s", state.archive_path.data());
  ImGui::TextWrapped("%s", state.archive_maintenance_status.data());
  if (state.archive_maintenance_busy)
    ImGui::BeginDisabled();
  if (ImGui::Button("Refresh archive health")) {
    command.action = DashboardAction::refresh_archive_health;
  }
  if (state.archive_recoverable_incident) {
    ImGui::SameLine();
    if (ImGui::Button("Retry failed incident")) {
      command.action = DashboardAction::retry_failed_incident;
    }
    ImGui::TextColored(
        ImVec4{1.0F, 0.75F, 0.25F, 1.0F},
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
  ImGui::InputText("New pre-restore safety backup",
                   product.safety_backup_path.data(),
                   product.safety_backup_path.size());
  ImGui::Checkbox("I understand restore replaces the active archive",
                  &product.restore_confirmed);
  const auto restore_disabled = !product.restore_confirmed;
  if (restore_disabled)
    ImGui::BeginDisabled();
  if (ImGui::Button("Validate source and restore")) {
    command.action = DashboardAction::restore_archive;
    command.restore_path = product.restore_path.data();
    command.safety_backup_path = product.safety_backup_path.data();
    product.restore_confirmed = false;
  }
  if (restore_disabled)
    ImGui::EndDisabled();

  ImGui::InputText("Dataset export directory", product.export_path.data(),
                   product.export_path.size());
  if (ImGui::Button("Export inspectable evidence dataset")) {
    command.action = DashboardAction::export_dataset;
    command.export_path = product.export_path.data();
  }
  if (state.archive_recoverable_incident) {
    ImGui::InputText("Failed incident export file",
                     product.failed_export_path.data(),
                     product.failed_export_path.size());
    if (ImGui::Button("Export failed incident before retry")) {
      command.action = DashboardAction::export_failed_incident;
      command.failed_export_path = product.failed_export_path.data();
    }
  }

  ImGui::InputScalar("Keep newest incidents", ImGuiDataType_U64,
                     &product.retention_incidents);
  ImGui::Checkbox("Confirm explicit retention deletion",
                  &product.retention_confirmed);
  const auto retention_disabled = !product.retention_confirmed;
  if (retention_disabled)
    ImGui::BeginDisabled();
  if (ImGui::Button("Apply retention now")) {
    command.action = DashboardAction::retain_incidents;
    command.retention_incidents =
        static_cast<std::size_t>(product.retention_incidents);
    product.retention_confirmed = false;
  }
  if (retention_disabled)
    ImGui::EndDisabled();

  ImGui::Checkbox("Confirm permanent privacy purge of incidents and profiles",
                  &product.purge_confirmed);
  const auto purge_disabled = !product.purge_confirmed;
  if (purge_disabled)
    ImGui::BeginDisabled();
  if (ImGui::Button("Purge all local incident evidence")) {
    command.action = DashboardAction::purge_archive;
    product.purge_confirmed = false;
  }
  if (purge_disabled)
    ImGui::EndDisabled();
  if (state.archive_maintenance_busy)
    ImGui::EndDisabled();
}

} // namespace blackbox::ui::detail
