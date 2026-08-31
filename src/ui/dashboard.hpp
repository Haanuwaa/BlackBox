#pragma once

#include "ui/incident_viewer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace blackbox::ui {

inline constexpr std::size_t dashboard_history_capacity = 300U;
inline constexpr std::size_t dashboard_process_capacity = 50U;
inline constexpr std::size_t product_path_capacity = 1'024U;

enum class ProductPage : std::uint8_t {
    live, incidents, detail, patterns, settings, diagnostics
};

struct ProductUiState {
    ProductPage page{ProductPage::live};
    bool onboarding_open{true};
    bool settings_initialized{};
    std::uint32_t hotkey_key{12U};
    bool hotkey_control{true};
    bool hotkey_shift{true};
    bool hotkey_alt{};
    bool hotkey_windows{};
    bool automatic_detection{true};
    int detector_sensitivity{1};
    bool detect_cpu{true};
    bool detect_memory{true};
    bool detect_disk{true};
    bool detect_network{true};
    std::uint64_t detector_cooldown_seconds{120U};
    bool notifications{true};
    bool record_foreground_application{};
    bool record_process_lifecycle{};
    bool record_power_and_device_events{};
    bool record_audio_device_events{};
    bool record_system_event_evidence{};
    bool collect_process_paths{true};
    std::uint64_t incident_pre_window_seconds{120U};
    std::uint64_t incident_post_window_seconds{30U};
    std::uint64_t archive_maximum_mib{1'024U};
    std::array<char, product_path_capacity + 1U> archive_path{};
    std::array<char, product_path_capacity + 1U> backup_path{};
    std::array<char, product_path_capacity + 1U> restore_path{};
    std::array<char, product_path_capacity + 1U> safety_backup_path{};
    std::array<char, product_path_capacity + 1U> export_path{};
    std::array<char, product_path_capacity + 1U> failed_export_path{};
    std::array<char, product_path_capacity + 1U> support_bundle_path{};
    bool include_latest_crash_evidence{};
    bool crash_evidence_consent_confirmed{};
    std::uint64_t retention_incidents{500U};
    bool restore_confirmed{};
    bool retention_confirmed{};
    bool purge_confirmed{};
    bool feedback_reset_confirmed{};
    bool timeline_initialized{};
    std::int64_t timeline_incident_id{};
    double timeline_min{};
    double timeline_max{};
    bool timeline_cursor_visible{};
    double timeline_cursor_seconds{};
};

enum class MetricDisplayStatus {
    available,
    warming_up,
    unsupported,
    inaccessible,
    unavailable,
};

enum class DashboardAction : std::uint8_t {
    none,
    capture_incident,
    refresh_incidents,
    select_incident,
    select_incident_process,
    save_incident_annotation,
    save_incident_feedback,
    save_contributor_feedback,
    refresh_recurring_incidents,
    save_recurring_group_override,
    reset_feedback_profile,
    rollback_feedback_profile_reset,
    apply_recorder_settings,
    apply_product_settings,
    complete_onboarding,
    refresh_archive_health,
    retry_failed_incident,
    backup_archive,
    restore_archive,
    retain_incidents,
    export_dataset,
    export_failed_incident,
    purge_archive,
    create_support_bundle,
};

struct DashboardCommand {
    DashboardAction action{DashboardAction::none};
    std::int64_t incident_id{};
    std::size_t incident_offset{};
    IncidentListOrder incident_order{IncidentListOrder::newest_first};
    core::IncidentProcessIdentity process_identity{};
    std::string search{};
    std::string label{};
    std::string note{};
    IncidentFeedback incident_feedback{IncidentFeedback::unanswered};
    IncidentCategory incident_category{IncidentCategory::unknown};
    std::string contributor_executable_key{};
    IncidentContributorRow::Resource contributor_resource{
        IncidentContributorRow::Resource::cpu};
    IncidentContributorRow::Attribution contributor_attribution{
        IncidentContributorRow::Attribution::unsure};
    IncidentContributorRow::TemporalRelationship contributor_temporal_relationship{
        IncidentContributorRow::TemporalRelationship::preceding_activity};
    std::string recurring_group_override{};
    std::uint64_t sample_interval_milliseconds{1'000U};
    std::uint64_t history_duration_seconds{300U};
    std::uint32_t hotkey_key{12U};
    bool hotkey_control{true};
    bool hotkey_shift{true};
    bool hotkey_alt{};
    bool hotkey_windows{};
    bool automatic_detection{true};
    int detector_sensitivity{1};
    bool detect_cpu{true};
    bool detect_memory{true};
    bool detect_disk{true};
    bool detect_network{true};
    std::uint64_t detector_cooldown_seconds{120U};
    bool notifications{true};
    bool record_foreground_application{};
    bool record_process_lifecycle{};
    bool record_power_and_device_events{};
    bool record_audio_device_events{};
    bool record_system_event_evidence{};
    bool collect_process_paths{true};
    std::uint64_t incident_pre_window_seconds{120U};
    std::uint64_t incident_post_window_seconds{30U};
    std::uint64_t archive_maximum_mib{1'024U};
    std::size_t retention_incidents{500U};
    std::string archive_path{};
    std::string backup_path{};
    std::string restore_path{};
    std::string safety_backup_path{};
    std::string export_path{};
    std::string failed_export_path{};
    std::string support_bundle_path{};
    bool include_latest_crash_evidence{};
    bool crash_evidence_disclosure_confirmed{};
};

struct ProcessRow {
    std::uint32_t pid{};
    std::string name{};
    std::string executable_path{};
    MetricDisplayStatus cpu_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus memory_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus disk_read_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus disk_write_status{MetricDisplayStatus::warming_up};
    double cpu_percent{};
    double working_set_mib{};
    double disk_read_mib_per_second{};
    double disk_write_mib_per_second{};
};

struct DashboardState {
    std::string recorder_status{"Stopped"};
    std::string platform_name{"Unknown"};
    std::string provider_name{"Not configured"};
    std::string provider_status{"Starting"};
    std::string renderer_backend{"Unavailable"};
    std::string hotkey_status{"Not configured"};
    std::string background_status{"Unavailable"};
    std::string incident_capture_status{"Stopped"};
    std::string storage_status{"Disabled"};
    std::string recorder_settings_status{"Conservative defaults"};
    std::string automatic_event_capture_status{
        "Unavailable until Windows event capabilities are known"};
    std::string automatic_frame_capture_status{
        "Unsupported: no bounded OS-wide frame-time source"};
    std::string automatic_audio_capture_status{
        "Unsupported: endpoint transitions do not prove an audio glitch"};
    bool incident_capture_enabled{};
    double incident_pre_window_seconds{120.0};
    double incident_post_window_seconds{30.0};
    double incident_post_remaining_seconds{};
    std::size_t incident_queue_size{};
    std::size_t incident_queue_capacity{};
    std::uint64_t incident_captures_started{};
    std::uint64_t incident_requests_merged{};
    std::uint64_t incidents_completed{};
    std::uint64_t incident_queue_rejections{};
    std::uint64_t incident_snapshot_failures{};
    std::uint64_t incident_captures_cancelled{};
    bool automatic_detection_enabled{};
    std::uint64_t automatic_detector_samples{};
    std::uint64_t automatic_detector_triggers{};
    std::uint64_t automatic_detector_cooldown_suppressions{};
    std::uint64_t automatic_detector_single_observation_triggers{};
    std::uint64_t automatic_capture_rejections{};
    std::uint64_t automatic_event_capture_requests{};
    std::uint64_t automatic_event_capture_rejections{};
    double incident_snapshot_average_microseconds{};
    double incident_snapshot_p95_microseconds{};
    double incident_snapshot_p99_microseconds{};
    double incident_snapshot_maximum_microseconds{};
    bool storage_writer_running{};
    bool storage_writing{};
    bool storage_retrying{};
    std::uint64_t storage_write_attempts{};
    std::uint64_t storage_retry_attempts{};
    std::uint64_t storage_retry_exhausted{};
    std::uint64_t storage_write_successes{};
    std::uint64_t storage_write_failures{};
    std::uint64_t storage_write_cancellations{};
    std::uint64_t stored_incident_count{};
    double storage_write_average_microseconds{};
    double storage_write_p95_microseconds{};
    double storage_write_p99_microseconds{};
    double storage_write_maximum_microseconds{};
    MetricDisplayStatus cpu_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus memory_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus disk_read_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus disk_write_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus network_receive_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus network_transmit_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus disk_latency_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus disk_queue_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus network_connectivity_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus network_transport_quality_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus gpu_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus gpu_memory_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus gpu_inventory_status{MetricDisplayStatus::unsupported};
    MetricDisplayStatus foreground_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus dpc_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus cpu_frequency_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus cpu_thermal_limit_status{MetricDisplayStatus::warming_up};
    MetricDisplayStatus power_status{MetricDisplayStatus::warming_up};
    double cpu_usage{};
    std::uint64_t memory_used_bytes{};
    std::uint64_t memory_total_bytes{};
    double memory_usage{};
    double disk_read_mib_per_second{};
    double disk_write_mib_per_second{};
    double network_receive_mib_per_second{};
    double network_transmit_mib_per_second{};
    double disk_read_latency_milliseconds{};
    double disk_write_latency_milliseconds{};
    double disk_service_time_milliseconds{};
    double disk_queue_depth{};
    std::uint64_t disk_worst_device_id{};
    std::uint8_t network_connectivity_level{4U};
    std::uint64_t network_active_interfaces{};
    std::uint64_t network_interface_changes{};
    double network_tcp_retransmit_percent{};
    std::uint64_t network_tcp_failed_connections{};
    std::uint64_t network_tcp_resets{};
    double gpu_usage{};
    double gpu_dedicated_memory_mib{};
    double gpu_shared_memory_mib{};
    std::uint32_t gpu_device_count{};
    std::uint32_t gpu_integrated_device_count{};
    std::uint32_t gpu_discrete_device_count{};
    std::uint32_t gpu_unknown_device_count{};
    bool gpu_render_device_available{};
    bool renderer_active{};
    std::uint32_t foreground_pid{};
    double foreground_gpu_usage{};
    double dpc_usage{};
    double interrupt_usage{};
    double dpc_rate{};
    double cpu_current_mhz{};
    double cpu_max_mhz{};
    double cpu_thermal_limit_mhz{};
    double cpu_thermal_limit_fraction{};
    std::uint8_t power_source{3U};
    double battery_fraction{};
    bool battery_saver{};
    bool event_collector_running{};
    std::uint64_t system_events_recorded{};
    std::uint64_t system_events_dropped{};
    std::uint64_t process_lifecycle_observations{};
    std::uint64_t process_lifecycle_events_recorded{};
    std::size_t system_event_ring_size{};
    std::size_t system_event_ring_capacity{};
    std::array<float, dashboard_history_capacity> cpu_history_x{};
    std::array<float, dashboard_history_capacity> cpu_history{};
    std::array<float, dashboard_history_capacity> memory_history_x{};
    std::array<float, dashboard_history_capacity> memory_history{};
    std::array<float, dashboard_history_capacity> disk_read_history_x{};
    std::array<float, dashboard_history_capacity> disk_read_history{};
    std::array<float, dashboard_history_capacity> disk_write_history_x{};
    std::array<float, dashboard_history_capacity> disk_write_history{};
    std::array<float, dashboard_history_capacity> network_receive_history_x{};
    std::array<float, dashboard_history_capacity> network_receive_history{};
    std::array<float, dashboard_history_capacity> network_transmit_history_x{};
    std::array<float, dashboard_history_capacity> network_transmit_history{};
    std::size_t history_size{};
    std::size_t cpu_history_points{};
    std::size_t memory_history_points{};
    std::size_t disk_read_history_points{};
    std::size_t disk_write_history_points{};
    std::size_t network_receive_history_points{};
    std::size_t network_transmit_history_points{};
    double disk_history_max_mib_per_second{1.0};
    double network_history_max_mib_per_second{1.0};
    std::array<ProcessRow, dashboard_process_capacity> processes{};
    std::size_t process_count{};
    std::size_t active_process_count{};
    std::size_t process_metadata_count{};
    std::size_t process_metadata_capacity{};
    std::uint64_t process_metadata_evictions{};
    std::uint64_t process_inaccessible{};
    std::uint64_t process_exits_during_sampling{};
    std::uint64_t process_samples_truncated{};
    std::uint64_t collection_count{};
    std::uint64_t partial_samples{};
    std::uint64_t failed_samples{};
    std::uint64_t dropped_samples{};
    std::uint64_t late_samples{};
    std::uint64_t deadline_misses{};
    std::uint64_t resume_events{};
    std::uint64_t resume_skipped_samples{};
    double last_resume_gap_seconds{};
    std::uint64_t provider_recoveries{};
    std::uint64_t consecutive_provider_failures{};
    std::uint64_t collector_worker_failures{};
    std::uint64_t ring_overwrites{};
    std::size_t ring_size{};
    std::size_t ring_capacity{};
    double ring_utilization{};
    double sample_interval_milliseconds{};
    double history_duration_seconds{};
    std::uint64_t timing_samples{};
    double timing_average_microseconds{};
    double timing_p50_microseconds{};
    double timing_p95_microseconds{};
    double timing_p99_microseconds{};
    double timing_maximum_microseconds{};
    double jitter_average_microseconds{};
    double jitter_p50_microseconds{};
    double jitter_p95_microseconds{};
    double jitter_p99_microseconds{};
    double jitter_maximum_microseconds{};
    bool archive_maintenance_busy{};
    bool accessibility_high_contrast{};
    bool accessibility_animations_enabled{true};
    double display_scale{1.0};
    std::uint32_t display_count{};
    std::uint32_t window_pixel_width{};
    std::uint32_t window_pixel_height{};
    bool archive_healthy{};
    bool archive_recoverable_incident{};
    std::uint64_t archive_recoverable_sequence{};
    std::uint64_t archive_database_size_bytes{};
    std::uint64_t archive_maximum_bytes{};
    std::int32_t archive_schema_version{};
    std::string archive_path{"Unavailable"};
    std::string archive_maintenance_status{"Not checked"};
    bool crash_diagnostics_available{};
    bool crash_diagnostics_armed{};
    bool latest_crash_evidence_available{};
    std::uint64_t previous_crash_evidence{};
    std::string crash_diagnostics_status{"Crash diagnostics unavailable"};
    bool support_bundle_busy{};
    std::string support_bundle_status{"Support bundle service is stopped"};
};

// Stores one finite marker-relative cursor shared by every incident-detail plot.
// Invalid ranges/values are rejected without changing the existing cursor.
[[nodiscard]] bool set_timeline_cursor(ProductUiState& product,
                                       double seconds_from_event,
                                       double incident_start_seconds,
                                       double incident_end_seconds) noexcept;
void clear_timeline_cursor(ProductUiState& product) noexcept;

[[nodiscard]] DashboardCommand render_dashboard(const DashboardState& state,
                                                 IncidentViewerState& incident_viewer,
                                                 ProductUiState& product);

} // namespace blackbox::ui
