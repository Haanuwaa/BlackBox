#include "app/application.hpp"
#include "app/dashboard_projection.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <unordered_map>

namespace blackbox::app {
namespace {

using namespace std::chrono_literals;

struct ProcessIdentityHash {
    [[nodiscard]] std::size_t operator()(const telemetry::ProcessIdentity identity) const noexcept {
        return std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(identity.pid.value) << 32U) ^
                                          identity.creation_token);
    }
};

[[nodiscard]] constexpr ui::MetricDisplayStatus
display_status(const telemetry::MetricStatus status) noexcept {
    switch (status) {
    case telemetry::MetricStatus::available:
        return ui::MetricDisplayStatus::available;
    case telemetry::MetricStatus::unsupported:
        return ui::MetricDisplayStatus::unsupported;
    case telemetry::MetricStatus::inaccessible:
        return ui::MetricDisplayStatus::inaccessible;
    case telemetry::MetricStatus::temporarily_unavailable:
        return ui::MetricDisplayStatus::warming_up;
    }
    return ui::MetricDisplayStatus::unavailable;
}

[[nodiscard]] constexpr std::string_view
provider_status_text(const telemetry::ProviderSampleStatus status) noexcept {
    switch (status) {
    case telemetry::ProviderSampleStatus::complete:
        return "Collecting";
    case telemetry::ProviderSampleStatus::partial:
        return "Partial sample";
    case telemetry::ProviderSampleStatus::temporarily_failed:
        return "Temporarily unavailable";
    }
    return "Unknown";
}

[[nodiscard]] constexpr double microseconds(const std::chrono::nanoseconds duration) noexcept {
    return std::chrono::duration<double, std::micro>{duration}.count();
}

[[nodiscard]] constexpr double milliseconds(const std::chrono::nanoseconds duration) noexcept {
    return std::chrono::duration<double, std::milli>{duration}.count();
}

[[nodiscard]] constexpr double seconds(const std::chrono::nanoseconds duration) noexcept {
    return std::chrono::duration<double>{duration}.count();
}

[[nodiscard]] constexpr double
mebibytes_per_second(const telemetry::BytesPerSecond value) noexcept {
    return value.value / (1024.0 * 1024.0);
}

[[nodiscard]] constexpr std::string_view
capture_phase_text(const core::IncidentCapturePhase phase) noexcept {
    switch (phase) {
    case core::IncidentCapturePhase::idle:
        return "Ready";
    case core::IncidentCapturePhase::collecting_post_window:
        return "Collecting post-window";
    case core::IncidentCapturePhase::constructing_snapshot:
        return "Constructing immutable snapshot";
    case core::IncidentCapturePhase::queued:
        return "Queued for incident writer";
    case core::IncidentCapturePhase::queue_full:
        return "Writer queue full";
    case core::IncidentCapturePhase::stopped:
        return "Stopped";
    }
    return "Unknown";
}

} // namespace

void Application::refresh_dashboard_if_due() {
    const auto now = telemetry_clock_.now();
    if (collector_ == nullptr || now < next_dashboard_refresh_at_) {
        return;
    }
    do {
        next_dashboard_refresh_at_ += 250ms;
    } while (next_dashboard_refresh_at_ <= now);

    refresh_accessibility_if_due();
    const auto diagnostics = collector_->diagnostics();
    constexpr std::uint64_t gpu_inventory_refresh_samples = 30U;
    if (dashboard_gpu_inventory_collection_count_ == std::numeric_limits<std::uint64_t>::max() ||
        diagnostics.collection_count < dashboard_gpu_inventory_collection_count_ ||
        diagnostics.collection_count - dashboard_gpu_inventory_collection_count_ >=
            gpu_inventory_refresh_samples) {
        const auto gpu_inventory = telemetry_provider_->gpu_inventory();
        dashboard_state_.gpu_inventory_status = display_status(gpu_inventory.device_count.status);
        if (gpu_inventory.device_count.has_value()) {
            dashboard_state_.gpu_device_count = gpu_inventory.device_count.value;
            dashboard_state_.gpu_integrated_device_count =
                gpu_inventory.integrated_device_count.value;
            dashboard_state_.gpu_discrete_device_count = gpu_inventory.discrete_device_count.value;
            dashboard_state_.gpu_unknown_device_count = gpu_inventory.unknown_device_count.value;
        }
        if (gpu_inventory.render_device_available.has_value()) {
            dashboard_state_.gpu_render_device_available =
                gpu_inventory.render_device_available.value;
        }
        dashboard_gpu_inventory_collection_count_ = diagnostics.collection_count;
    }
    dashboard_state_.display_scale = display_scale_;
    dashboard_state_.recorder_status = diagnostics.running ? "Recording" : "Stopped";
    dashboard_state_.provider_status = provider_status_text(diagnostics.provider_status);
    dashboard_state_.collection_count = diagnostics.collection_count;
    dashboard_state_.partial_samples = diagnostics.partial_samples;
    dashboard_state_.failed_samples = diagnostics.failed_samples;
    dashboard_state_.dropped_samples = diagnostics.dropped_samples;
    dashboard_state_.late_samples = diagnostics.late_samples;
    dashboard_state_.deadline_misses = diagnostics.deadline_misses;
    dashboard_state_.resume_events = diagnostics.resume_events;
    dashboard_state_.resume_skipped_samples = diagnostics.resume_skipped_samples;
    dashboard_state_.last_resume_gap_seconds = seconds(diagnostics.last_resume_gap);
    dashboard_state_.provider_recoveries = diagnostics.provider_recoveries;
    dashboard_state_.consecutive_provider_failures = diagnostics.consecutive_provider_failures;
    dashboard_state_.collector_worker_failures = diagnostics.worker_failures;
    dashboard_state_.ring_size = diagnostics.ring.size;
    dashboard_state_.ring_capacity = diagnostics.ring.capacity;
    dashboard_state_.ring_overwrites = diagnostics.ring.overwritten_samples;
    dashboard_state_.ring_utilization = diagnostics.ring.utilization();
    dashboard_state_.sample_interval_milliseconds =
        milliseconds(diagnostics.configuration.sample_interval);
    dashboard_state_.history_duration_seconds = seconds(diagnostics.configuration.history_duration);
    dashboard_state_.timing_samples = diagnostics.collection_timing.samples_recorded;
    dashboard_state_.timing_average_microseconds =
        microseconds(diagnostics.collection_timing.average);
    dashboard_state_.timing_p50_microseconds = microseconds(diagnostics.collection_timing.p50);
    dashboard_state_.timing_p95_microseconds = microseconds(diagnostics.collection_timing.p95);
    dashboard_state_.timing_p99_microseconds = microseconds(diagnostics.collection_timing.p99);
    dashboard_state_.timing_maximum_microseconds =
        microseconds(diagnostics.collection_timing.maximum);
    dashboard_state_.jitter_average_microseconds =
        microseconds(diagnostics.scheduling_jitter.average);
    dashboard_state_.jitter_p50_microseconds = microseconds(diagnostics.scheduling_jitter.p50);
    dashboard_state_.jitter_p95_microseconds = microseconds(diagnostics.scheduling_jitter.p95);
    dashboard_state_.jitter_p99_microseconds = microseconds(diagnostics.scheduling_jitter.p99);
    dashboard_state_.jitter_maximum_microseconds =
        microseconds(diagnostics.scheduling_jitter.maximum);
    dashboard_state_.active_process_count = diagnostics.active_processes;
    dashboard_state_.process_metadata_count = diagnostics.process_metadata_entries;
    dashboard_state_.process_metadata_capacity = diagnostics.process_metadata_capacity;
    dashboard_state_.process_metadata_evictions = diagnostics.process_metadata_evictions;
    dashboard_state_.process_inaccessible = diagnostics.process_inaccessible;
    dashboard_state_.process_exits_during_sampling = diagnostics.processes_exited_during_sample;
    dashboard_state_.process_samples_truncated = diagnostics.process_samples_truncated;
    dashboard_state_.process_lifecycle_observations = diagnostics.process_lifecycle_observations;
    dashboard_state_.process_lifecycle_events_recorded =
        diagnostics.process_lifecycle_events_recorded;
    if (system_event_collector_ != nullptr) {
        const auto events = system_event_collector_->diagnostics();
        dashboard_state_.event_collector_running = events.running;
        dashboard_state_.system_events_recorded = events.events_recorded;
        dashboard_state_.system_events_dropped =
            events.native_events_dropped + events.ring.overwritten_samples;
        dashboard_state_.system_event_ring_size = events.ring.size;
        dashboard_state_.system_event_ring_capacity = events.ring.capacity;
        dashboard_state_.automatic_event_capture_requests = events.automatic_event_requests;
        dashboard_state_.automatic_event_capture_rejections =
            events.automatic_event_capture_rejections;
        if (!product_settings_.automatic_detection_enabled) {
            dashboard_state_.automatic_event_capture_status =
                "Disabled with automatic incident detection";
#if defined(_WIN32)
        } else if (!event_provider_configuration(product_settings_).application_events ||
                   !event_provider_configuration(product_settings_).graphics_events ||
                   !event_provider_configuration(product_settings_).storage_events) {
            dashboard_state_.automatic_event_capture_status =
                "Disabled: Windows Event Log evidence recording is off";
        } else if (events.capabilities.application_events && events.capabilities.graphics_events &&
                   events.capabilities.storage_events) {
            dashboard_state_.automatic_event_capture_status =
                "Supported: Application crash 1000, Hang 1002, Display "
                "recovery "
                "4101, and Disk retry 153";
        } else {
            dashboard_state_.automatic_event_capture_status =
                "Partially unavailable: a system-event subscription failed";
#else
        } else if (!event_provider_configuration(product_settings_).application_events &&
                   !event_provider_configuration(product_settings_).network_events &&
                   !event_provider_configuration(product_settings_).graphics_events &&
                   !event_provider_configuration(product_settings_).storage_events) {
            dashboard_state_.automatic_event_capture_status =
                "Disabled: privacy-bounded system event recording is off";
        } else if (events.capabilities.application_events || events.capabilities.network_events ||
                   events.capabilities.graphics_events || events.capabilities.storage_events) {
            dashboard_state_.automatic_event_capture_status =
                "Evidence enabled: native symptom and context sources are "
                "capability-gated";
        } else {
            dashboard_state_.automatic_event_capture_status =
                "Unavailable: native system-event sources could not be opened";
#endif
        }
    }
    refresh_hotkey_status();
    dashboard_state_.recorder_settings_status = recorder_settings_status_text_;
    dashboard_state_.incident_capture_status =
        capture_phase_text(diagnostics.incident_capture.phase);
    dashboard_state_.incident_capture_enabled = diagnostics.incident_capture.can_request;
    dashboard_state_.incident_pre_window_seconds =
        seconds(diagnostics.configuration.incident_pre_window);
    dashboard_state_.incident_post_window_seconds =
        seconds(diagnostics.configuration.incident_post_window);
    dashboard_state_.incident_post_remaining_seconds = 0.0;
    if (diagnostics.incident_capture.has_pending_window &&
        diagnostics.incident_capture.pending_window.requested_end > now) {
        dashboard_state_.incident_post_remaining_seconds =
            seconds(diagnostics.incident_capture.pending_window.requested_end - now);
    }
    dashboard_state_.incident_queue_size = diagnostics.incident_capture.queue_size;
    dashboard_state_.incident_queue_capacity = diagnostics.incident_capture.queue_capacity;
    dashboard_state_.incident_captures_started = diagnostics.incident_capture.captures_started;
    dashboard_state_.incident_requests_merged =
        diagnostics.incident_capture.capture_requests_merged;
    dashboard_state_.incidents_completed = diagnostics.incident_capture.incidents_completed;
    dashboard_state_.incident_queue_rejections = diagnostics.incident_capture.queue_rejections;
    dashboard_state_.incident_snapshot_failures = diagnostics.incident_capture.snapshot_failures;
    dashboard_state_.incident_captures_cancelled = diagnostics.incident_capture.captures_cancelled;
    dashboard_state_.automatic_detection_enabled = diagnostics.automatic_detection_enabled;
    dashboard_state_.automatic_detector_samples = diagnostics.automatic_detector.samples_observed;
    dashboard_state_.automatic_detector_triggers = diagnostics.automatic_detector.triggers_emitted;
    dashboard_state_.automatic_detector_cooldown_suppressions =
        diagnostics.automatic_detector.triggers_suppressed_by_cooldown;
    dashboard_state_.automatic_detector_single_observation_triggers =
        diagnostics.automatic_detector.single_observation_triggers;
    dashboard_state_.automatic_capture_rejections =
        diagnostics.automatic_capture_rejections +
        dashboard_state_.automatic_event_capture_rejections;
    dashboard_state_.incident_snapshot_average_microseconds =
        microseconds(diagnostics.incident_snapshot_timing.average);
    dashboard_state_.incident_snapshot_p95_microseconds =
        microseconds(diagnostics.incident_snapshot_timing.p95);
    dashboard_state_.incident_snapshot_p99_microseconds =
        microseconds(diagnostics.incident_snapshot_timing.p99);
    dashboard_state_.incident_snapshot_maximum_microseconds =
        microseconds(diagnostics.incident_snapshot_timing.maximum);
#if BLACKBOX_STORAGE_ENABLED
    if (incident_writer_ != nullptr) {
        const auto writer = incident_writer_->diagnostics();
        if (writer.retrying) {
            storage_status_text_ = "Retrying incident persistence: " + writer.last_error_message;
        } else if (writer.state == storage::WriterState::degraded) {
            storage_status_text_ = "Degraded: " + writer.last_error_message;
        } else if (writer.state == storage::WriterState::running) {
            storage_status_text_ = "Ready";
        }
        dashboard_state_.storage_writer_running = writer.state == storage::WriterState::running ||
                                                  writer.state == storage::WriterState::degraded;
        dashboard_state_.storage_writing = writer.writing;
        dashboard_state_.storage_retrying = writer.retrying;
        dashboard_state_.storage_write_attempts = writer.attempts;
        dashboard_state_.storage_retry_attempts = writer.retry_attempts;
        dashboard_state_.storage_retry_exhausted = writer.retry_exhausted;
        dashboard_state_.storage_write_successes = writer.succeeded;
        dashboard_state_.storage_write_failures = writer.failed;
        dashboard_state_.storage_write_cancellations = writer.cancelled;
        dashboard_state_.stored_incident_count = stored_incidents_at_start_ + writer.succeeded;
        dashboard_state_.storage_write_average_microseconds =
            microseconds(writer.write_timing.average);
        dashboard_state_.storage_write_p95_microseconds = microseconds(writer.write_timing.p95);
        dashboard_state_.storage_write_p99_microseconds = microseconds(writer.write_timing.p99);
        dashboard_state_.storage_write_maximum_microseconds =
            microseconds(writer.write_timing.maximum);
        if (writer.succeeded != viewer_last_writer_successes_) {
            viewer_last_writer_successes_ = writer.succeeded;
            if (archive_maintenance_service_ != nullptr) {
                archive_maintenance_service_->refresh();
            }
            if (incident_viewer_service_ != nullptr) {
                incident_viewer_service_->request_page(0U, incident_viewer_state_.search.data(),
                                                       incident_viewer_state_.order);
                incident_viewer_service_->request_recurring_incidents();
            }
        }
    }
    if (incident_viewer_service_ != nullptr) {
        incident_viewer_state_.content = incident_viewer_service_->snapshot();
        const auto queue = incident_viewer_service_->queue_diagnostics();
        dashboard_state_.viewer_read_queue_depth = queue.queued_reads;
        dashboard_state_.viewer_mutation_queue_depth = queue.queued_mutations;
        dashboard_state_.viewer_reads_coalesced = queue.coalesced_reads;
        dashboard_state_.viewer_reads_cancelled = queue.cancelled_reads;
        dashboard_state_.viewer_mutations_rejected = queue.rejected_mutations;
        dashboard_state_.viewer_mutations_completed = queue.completed_mutations;
    }
    if (archive_maintenance_service_ != nullptr) {
        const auto maintenance = archive_maintenance_service_->snapshot();
        dashboard_state_.archive_maintenance_busy = maintenance->busy;
        dashboard_state_.archive_healthy = maintenance->healthy;
        dashboard_state_.archive_recoverable_incident = maintenance->recoverable_incident;
        dashboard_state_.archive_recoverable_sequence = maintenance->recoverable_capture_sequence;
        dashboard_state_.archive_database_size_bytes = maintenance->database_size_bytes;
        dashboard_state_.archive_maximum_bytes = maintenance->maximum_bytes;
        dashboard_state_.archive_schema_version = maintenance->schema_version;
        dashboard_state_.archive_path = maintenance->archive_path;
        dashboard_state_.archive_maintenance_status = maintenance->status;
        if (!maintenance->busy &&
            maintenance->incident_count != dashboard_state_.stored_incident_count) {
            dashboard_state_.stored_incident_count = maintenance->incident_count;
        }
        if (!maintenance->busy && maintenance->generation != archive_maintenance_generation_) {
            archive_maintenance_generation_ = maintenance->generation;
            if (incident_viewer_service_ != nullptr) {
                incident_viewer_service_->request_page(0U, incident_viewer_state_.search.data(),
                                                       incident_viewer_state_.order);
                incident_viewer_service_->request_recurring_incidents();
            }
        }
    }
#endif
    dashboard_state_.storage_status = storage_status_text_;

    const auto support = support_bundle_service_.snapshot();
    dashboard_state_.support_bundle_busy = support->busy;
    dashboard_state_.support_bundle_status = support->status;
    if (crash_diagnostics_ != nullptr) {
        const auto crash = crash_diagnostics_->snapshot();
        dashboard_state_.crash_diagnostics_available = crash.available;
        dashboard_state_.crash_diagnostics_armed = crash.armed;
        dashboard_state_.previous_crash_evidence = crash.completed_evidence;
        dashboard_state_.latest_crash_evidence_available = !crash.latest_evidence.empty();
        dashboard_state_.crash_diagnostics_status = crash.status;
    }

    if (dashboard_projection_collection_count_ == diagnostics.collection_count) {
        return;
    }
    dashboard_projection_collection_count_ = diagnostics.collection_count;
    const auto snapshot = collector_->snapshot(ui::dashboard_history_capacity);
    auto active_processes = collector_->active_process_snapshot();

    dashboard_state_.process_count =
        std::min(active_processes.frame.processes.size(), ui::dashboard_process_capacity);
    select_top_dashboard_processes(active_processes.frame.processes,
                                   dashboard_state_.process_count);
    std::unordered_map<telemetry::ProcessIdentity, const telemetry::ProcessInfo*,
                       ProcessIdentityHash>
        metadata_by_identity;
    metadata_by_identity.reserve(active_processes.metadata.size());
    for (const auto& metadata : active_processes.metadata) {
        metadata_by_identity.emplace(metadata.identity, &metadata);
    }
    for (std::size_t index = 0U; index < dashboard_state_.process_count; ++index) {
        const auto& process = active_processes.frame.processes[index];
        auto& row = dashboard_state_.processes[index];
        row.name.clear();
        row.executable_path.clear();
        row.pid = process.identity.pid.value;
        row.cpu_status = display_status(process.cpu_usage.status);
        row.memory_status = display_status(process.working_set.status);
        row.disk_read_status = display_status(process.disk_read_rate.status);
        row.disk_write_status = display_status(process.disk_write_rate.status);
        row.cpu_percent = 0.0;
        row.working_set_mib = 0.0;
        row.disk_read_mib_per_second = 0.0;
        row.disk_write_mib_per_second = 0.0;
        if (process.cpu_usage.has_value()) {
            row.cpu_percent = process.cpu_usage.value.value * 100.0;
        }
        if (process.working_set.has_value()) {
            row.working_set_mib =
                static_cast<double>(process.working_set.value.value) / (1024.0 * 1024.0);
        }
        if (process.disk_read_rate.has_value()) {
            row.disk_read_mib_per_second = mebibytes_per_second(process.disk_read_rate.value);
        }
        if (process.disk_write_rate.has_value()) {
            row.disk_write_mib_per_second = mebibytes_per_second(process.disk_write_rate.value);
        }
        const auto metadata = metadata_by_identity.find(process.identity);
        if (metadata != metadata_by_identity.end()) {
            row.name = metadata->second->name.has_value() ? metadata->second->name.value
                                                          : "<name unavailable>";
            if (metadata->second->executable_path.has_value()) {
                row.executable_path = metadata->second->executable_path.value;
            }
        } else {
            row.name = "<metadata pending>";
        }
    }

    const auto unavailable = std::numeric_limits<float>::quiet_NaN();
    dashboard_state_.cpu_history.fill(unavailable);
    dashboard_state_.memory_history.fill(unavailable);
    dashboard_state_.disk_read_history.fill(unavailable);
    dashboard_state_.disk_write_history.fill(unavailable);
    dashboard_state_.network_receive_history.fill(unavailable);
    dashboard_state_.network_transmit_history.fill(unavailable);
    dashboard_state_.history_size = snapshot.size();
    dashboard_state_.cpu_history_points = 0U;
    dashboard_state_.memory_history_points = 0U;
    dashboard_state_.disk_read_history_points = 0U;
    dashboard_state_.disk_write_history_points = 0U;
    dashboard_state_.network_receive_history_points = 0U;
    dashboard_state_.network_transmit_history_points = 0U;
    dashboard_state_.disk_history_max_mib_per_second = 1.0;
    dashboard_state_.network_history_max_mib_per_second = 1.0;
    for (std::size_t index = 0U; index < snapshot.size(); ++index) {
        const auto& sample = snapshot.samples()[index];
        if (sample.cpu_usage.has_value()) {
            const auto point = dashboard_state_.cpu_history_points++;
            dashboard_state_.cpu_history_x[point] = static_cast<float>(index);
            dashboard_state_.cpu_history[point] =
                static_cast<float>(sample.cpu_usage.value.value * 100.0);
        }
        if (sample.memory_usage.has_value()) {
            const auto point = dashboard_state_.memory_history_points++;
            dashboard_state_.memory_history_x[point] = static_cast<float>(index);
            dashboard_state_.memory_history[point] =
                static_cast<float>(sample.memory_usage.value.value * 100.0);
        }
        if (sample.disk_read_rate.has_value()) {
            const auto point = dashboard_state_.disk_read_history_points++;
            const auto value = mebibytes_per_second(sample.disk_read_rate.value);
            dashboard_state_.disk_read_history_x[point] = static_cast<float>(index);
            dashboard_state_.disk_read_history[point] = static_cast<float>(value);
            dashboard_state_.disk_history_max_mib_per_second =
                std::max(dashboard_state_.disk_history_max_mib_per_second, value);
        }
        if (sample.disk_write_rate.has_value()) {
            const auto point = dashboard_state_.disk_write_history_points++;
            const auto value = mebibytes_per_second(sample.disk_write_rate.value);
            dashboard_state_.disk_write_history_x[point] = static_cast<float>(index);
            dashboard_state_.disk_write_history[point] = static_cast<float>(value);
            dashboard_state_.disk_history_max_mib_per_second =
                std::max(dashboard_state_.disk_history_max_mib_per_second, value);
        }
        if (sample.network_receive_rate.has_value()) {
            const auto point = dashboard_state_.network_receive_history_points++;
            const auto value = mebibytes_per_second(sample.network_receive_rate.value);
            dashboard_state_.network_receive_history_x[point] = static_cast<float>(index);
            dashboard_state_.network_receive_history[point] = static_cast<float>(value);
            dashboard_state_.network_history_max_mib_per_second =
                std::max(dashboard_state_.network_history_max_mib_per_second, value);
        }
        if (sample.network_transmit_rate.has_value()) {
            const auto point = dashboard_state_.network_transmit_history_points++;
            const auto value = mebibytes_per_second(sample.network_transmit_rate.value);
            dashboard_state_.network_transmit_history_x[point] = static_cast<float>(index);
            dashboard_state_.network_transmit_history[point] = static_cast<float>(value);
            dashboard_state_.network_history_max_mib_per_second =
                std::max(dashboard_state_.network_history_max_mib_per_second, value);
        }
    }

    if (!snapshot.empty()) {
        const auto& latest = snapshot.samples().back();
        dashboard_state_.cpu_status = display_status(latest.cpu_usage.status);
        if (latest.cpu_usage.has_value()) {
            dashboard_state_.cpu_usage = latest.cpu_usage.value.value;
        }
        dashboard_state_.memory_status = display_status(latest.memory_usage.status);
        if (latest.memory_usage.has_value() && latest.memory_used.has_value() &&
            latest.memory_total.has_value()) {
            dashboard_state_.memory_usage = latest.memory_usage.value.value;
            dashboard_state_.memory_used_bytes = latest.memory_used.value.value;
            dashboard_state_.memory_total_bytes = latest.memory_total.value.value;
        }
        dashboard_state_.disk_read_status = display_status(latest.disk_read_rate.status);
        dashboard_state_.disk_write_status = display_status(latest.disk_write_rate.status);
        dashboard_state_.network_receive_status =
            display_status(latest.network_receive_rate.status);
        dashboard_state_.network_transmit_status =
            display_status(latest.network_transmit_rate.status);
        dashboard_state_.disk_latency_status = display_status(latest.disk_service_time.status);
        dashboard_state_.disk_queue_status = display_status(latest.disk_queue_depth.status);
        dashboard_state_.network_connectivity_status =
            display_status(latest.network_connectivity.status);
        dashboard_state_.network_transport_quality_status =
            display_status(latest.network_tcp_retransmit_fraction.status);
        dashboard_state_.gpu_status = display_status(latest.gpu_usage.status);
        dashboard_state_.gpu_memory_status = display_status(latest.gpu_dedicated_memory.status);
        dashboard_state_.foreground_status = display_status(latest.foreground_process.status);
        dashboard_state_.dpc_status = display_status(latest.dpc_usage.status);
        dashboard_state_.cpu_frequency_status = display_status(latest.cpu_current_mhz.status);
        dashboard_state_.cpu_thermal_limit_status =
            display_status(latest.cpu_thermal_limit_fraction.status);
        dashboard_state_.power_status = display_status(latest.power_source.status);
        dashboard_state_.cpu_some_pressure_status = display_status(latest.cpu_some_pressure.status);
        dashboard_state_.memory_some_pressure_status =
            display_status(latest.memory_some_pressure.status);
        dashboard_state_.memory_full_pressure_status =
            display_status(latest.memory_full_pressure.status);
        dashboard_state_.io_some_pressure_status = display_status(latest.io_some_pressure.status);
        dashboard_state_.io_full_pressure_status = display_status(latest.io_full_pressure.status);
        dashboard_state_.thermal_pressure_status =
            display_status(latest.thermal_pressure_state.status);
        if (latest.disk_read_rate.has_value()) {
            dashboard_state_.disk_read_mib_per_second =
                mebibytes_per_second(latest.disk_read_rate.value);
        }
        if (latest.disk_write_rate.has_value()) {
            dashboard_state_.disk_write_mib_per_second =
                mebibytes_per_second(latest.disk_write_rate.value);
        }
        if (latest.network_receive_rate.has_value()) {
            dashboard_state_.network_receive_mib_per_second =
                mebibytes_per_second(latest.network_receive_rate.value);
        }
        if (latest.network_transmit_rate.has_value()) {
            dashboard_state_.network_transmit_mib_per_second =
                mebibytes_per_second(latest.network_transmit_rate.value);
        }
        if (latest.disk_read_latency.has_value()) {
            dashboard_state_.disk_read_latency_milliseconds =
                latest.disk_read_latency.value.value * 1'000.0;
        }
        if (latest.disk_write_latency.has_value()) {
            dashboard_state_.disk_write_latency_milliseconds =
                latest.disk_write_latency.value.value * 1'000.0;
        }
        if (latest.disk_service_time.has_value()) {
            dashboard_state_.disk_service_time_milliseconds =
                latest.disk_service_time.value.value * 1'000.0;
        }
        if (latest.disk_queue_depth.has_value()) {
            dashboard_state_.disk_queue_depth = latest.disk_queue_depth.value;
        }
        if (latest.disk_worst_device_id.has_value()) {
            dashboard_state_.disk_worst_device_id = latest.disk_worst_device_id.value;
        }
        if (latest.network_connectivity.has_value()) {
            dashboard_state_.network_connectivity_level =
                static_cast<std::uint8_t>(latest.network_connectivity.value);
        }
        if (latest.network_active_interfaces.has_value()) {
            dashboard_state_.network_active_interfaces = latest.network_active_interfaces.value;
        }
        if (latest.network_interface_changes.has_value()) {
            dashboard_state_.network_interface_changes = latest.network_interface_changes.value;
        }
        if (latest.network_tcp_retransmit_fraction.has_value()) {
            dashboard_state_.network_tcp_retransmit_percent =
                latest.network_tcp_retransmit_fraction.value.value * 100.0;
        }
        if (latest.network_tcp_failed_connections.has_value()) {
            dashboard_state_.network_tcp_failed_connections =
                latest.network_tcp_failed_connections.value;
        }
        if (latest.network_tcp_resets.has_value()) {
            dashboard_state_.network_tcp_resets = latest.network_tcp_resets.value;
        }
        if (latest.gpu_usage.has_value()) {
            dashboard_state_.gpu_usage = latest.gpu_usage.value.value;
        }
        if (latest.gpu_dedicated_memory.has_value()) {
            dashboard_state_.gpu_dedicated_memory_mib =
                static_cast<double>(latest.gpu_dedicated_memory.value.value) / (1024.0 * 1024.0);
        }
        if (latest.gpu_shared_memory.has_value()) {
            dashboard_state_.gpu_shared_memory_mib =
                static_cast<double>(latest.gpu_shared_memory.value.value) / (1024.0 * 1024.0);
        }
        if (latest.foreground_process.has_value()) {
            dashboard_state_.foreground_pid = latest.foreground_process.value.pid.value;
        }
        if (latest.foreground_gpu_usage.has_value()) {
            dashboard_state_.foreground_gpu_usage = latest.foreground_gpu_usage.value.value;
        }
        if (latest.dpc_usage.has_value()) {
            dashboard_state_.dpc_usage = latest.dpc_usage.value.value;
        }
        if (latest.interrupt_usage.has_value()) {
            dashboard_state_.interrupt_usage = latest.interrupt_usage.value.value;
        }
        if (latest.dpc_rate.has_value()) {
            dashboard_state_.dpc_rate = latest.dpc_rate.value;
        }
        if (latest.cpu_current_mhz.has_value()) {
            dashboard_state_.cpu_current_mhz = latest.cpu_current_mhz.value;
        }
        if (latest.cpu_max_mhz.has_value()) {
            dashboard_state_.cpu_max_mhz = latest.cpu_max_mhz.value;
        }
        if (latest.cpu_thermal_limit_mhz.has_value()) {
            dashboard_state_.cpu_thermal_limit_mhz = latest.cpu_thermal_limit_mhz.value;
        }
        if (latest.cpu_thermal_limit_fraction.has_value()) {
            dashboard_state_.cpu_thermal_limit_fraction =
                latest.cpu_thermal_limit_fraction.value.value;
        }
        if (latest.power_source.has_value()) {
            dashboard_state_.power_source = static_cast<std::uint8_t>(latest.power_source.value);
        }
        if (latest.battery_fraction.has_value()) {
            dashboard_state_.battery_fraction = latest.battery_fraction.value.value;
        }
        if (latest.battery_saver.has_value()) {
            dashboard_state_.battery_saver = latest.battery_saver.value;
        }
        if (latest.cpu_some_pressure.has_value()) {
            dashboard_state_.cpu_some_pressure = latest.cpu_some_pressure.value.value;
        }
        if (latest.memory_some_pressure.has_value()) {
            dashboard_state_.memory_some_pressure = latest.memory_some_pressure.value.value;
        }
        if (latest.memory_full_pressure.has_value()) {
            dashboard_state_.memory_full_pressure = latest.memory_full_pressure.value.value;
        }
        if (latest.io_some_pressure.has_value()) {
            dashboard_state_.io_some_pressure = latest.io_some_pressure.value.value;
        }
        if (latest.io_full_pressure.has_value()) {
            dashboard_state_.io_full_pressure = latest.io_full_pressure.value.value;
        }
        if (latest.thermal_pressure_state.has_value()) {
            dashboard_state_.thermal_pressure_state =
                static_cast<std::uint8_t>(latest.thermal_pressure_state.value);
        }
    }
}

} // namespace blackbox::app
