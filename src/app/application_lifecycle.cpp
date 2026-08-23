#include "app/application.hpp"

#include "core/logger.hpp"
#include "core/version.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <implot.h>

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <string>

namespace blackbox::app {

void Application::shutdown() noexcept {
  if (shutdown_started_) {
    return;
  }
  shutdown_started_ = true;
  support_bundle_service_.stop();
  if (background_shell_ != nullptr) {
    final_shell_diagnostics_ = background_shell_->diagnostics();
    background_shell_->stop();
    background_shell_.reset();
    background_shell_started_ = false;
  }
  if (hotkey_manager_ != nullptr) {
    hotkey_manager_->unregister_hotkey();
    hotkey_manager_.reset();
  }
  if (system_event_collector_ != nullptr) {
    system_event_collector_->stop();
  }
  if (collector_ != nullptr) {
    collector_->stop();
  }
#if BLACKBOX_STORAGE_ENABLED
  if (archive_maintenance_service_ != nullptr) {
    archive_maintenance_service_->stop();
  }
  if (incident_viewer_service_ != nullptr) {
    incident_viewer_service_->stop();
  }
  if (incident_writer_ != nullptr) {
    incident_writer_->stop(storage::WriterStopPolicy::drain);
  }
  write_diagnostic_report();
  if (incident_archive_ != nullptr) {
    incident_archive_->close();
  }
#else
  write_diagnostic_report();
#endif
  if (imgui_renderer_backend_initialized_) {
    ImGui_ImplSDLRenderer3_Shutdown();
    imgui_renderer_backend_initialized_ = false;
  }
  if (imgui_sdl_backend_initialized_) {
    ImGui_ImplSDL3_Shutdown();
    imgui_sdl_backend_initialized_ = false;
  }
  if (implot_initialized_) {
    ImPlot::DestroyContext();
    implot_initialized_ = false;
  }
  if (imgui_initialized_) {
    ImGui::DestroyContext();
    imgui_initialized_ = false;
  }
  if (renderer_ != nullptr) {
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
  }
  if (window_ != nullptr) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_Quit();
    sdl_initialized_ = false;
  }
  if (crash_diagnostics_ != nullptr) {
    final_crash_diagnostics_ = crash_diagnostics_->snapshot();
  }
}

void Application::write_diagnostic_report() noexcept {
  if (!diagnostic_started_ || diagnostic_report_written_ ||
      diagnostic_options_.report_path.empty()) {
    return;
  }

  WallClockReport report{};
  report.application_version = std::string{core::version};
  report.platform = BLACKBOX_PLATFORM_NAME;
  if (const auto* video_driver = SDL_GetCurrentVideoDriver();
      video_driver != nullptr) {
    report.video_driver = video_driver;
  } else {
    report.video_driver = "unavailable";
  }
  report.source_revision = std::string{core::source_revision};
  report.completed = diagnostic_completed_;
  report.requested_runtime_seconds =
      static_cast<std::uint64_t>(diagnostic_options_.runtime.count());
  report.capture_interval_seconds =
      static_cast<std::uint64_t>(diagnostic_options_.capture_interval.count());

  if (collector_ != nullptr) {
    const auto values = collector_->diagnostics();
    const auto nanoseconds = [](const std::chrono::nanoseconds duration) {
      return duration.count() > 0 ? static_cast<std::uint64_t>(duration.count())
                                  : 0U;
    };
    report.collections = values.collection_count;
    report.partial_samples = values.partial_samples;
    report.failed_samples = values.failed_samples;
    report.dropped_samples = values.dropped_samples;
    report.late_samples = values.late_samples;
    report.deadline_misses = values.deadline_misses;
    const auto scheduling = collector_->scheduling_drop_snapshot();
    const auto scheduling_event_count = std::min(
        scheduling.events.size(),
        wall_clock_scheduling_drop_event_capacity);
    report.scheduling_drop_events.reserve(scheduling_event_count);
    report.scheduling_drop_event_overflow =
        scheduling.overflow;
    for (std::size_t index = 0U;
         index < scheduling_event_count; ++index) {
      const auto& source = scheduling.events[index];
      const auto utc = diagnostic_utc_anchor_ +
                       (source.observed_at - diagnostic_monotonic_anchor_);
      const auto utc_nanoseconds = std::chrono::duration_cast<
          std::chrono::nanoseconds>(utc.time_since_epoch()).count();
      report.scheduling_drop_events.push_back(WallClockSchedulingDropEvent{
          source.collection_index,
          utc_nanoseconds > 0 ? static_cast<std::uint64_t>(utc_nanoseconds) : 0U,
          nanoseconds(source.deadline_overrun), source.dropped_ticks});
    }
    report.resume_events = values.resume_events;
    report.resume_skipped_samples = values.resume_skipped_samples;
    report.provider_recoveries = values.provider_recoveries;
    report.collector_worker_failures = values.worker_failures;
    report.collection_p99_nanoseconds =
        nanoseconds(values.collection_timing.p99);
    report.collection_maximum_nanoseconds =
        nanoseconds(values.collection_timing.maximum);
    report.jitter_p99_nanoseconds = nanoseconds(values.scheduling_jitter.p99);
    report.jitter_maximum_nanoseconds =
        nanoseconds(values.scheduling_jitter.maximum);
    report.ring_capacity = values.ring.capacity;
    report.ring_size = values.ring.size;
    report.ring_total_appends = values.ring.total_appends;
    report.ring_overwritten_samples = values.ring.overwritten_samples;
    report.ring_discarded_samples = values.ring.discarded_samples;
    report.process_ring_capacity = values.process_ring.capacity;
    report.process_ring_size = values.process_ring.size;
    report.process_ring_total_appends = values.process_ring.total_appends;
    report.process_ring_overwritten_samples =
        values.process_ring.overwritten_samples;
    report.process_ring_discarded_samples =
        values.process_ring.discarded_samples;
    report.captures_started = values.incident_capture.captures_started;
    report.capture_requests_merged =
        values.incident_capture.capture_requests_merged;
    report.incidents_completed = values.incident_capture.incidents_completed;
    report.capture_queue_rejections = values.incident_capture.queue_rejections;
    report.snapshot_failures = values.incident_capture.snapshot_failures;
    report.captures_cancelled = values.incident_capture.captures_cancelled;
    report.automatic_detection_enabled = values.automatic_detection_enabled;
    report.automatic_detector_triggers =
        values.automatic_detector.triggers_emitted;
    report.automatic_captures_started = values.automatic_captures_started;
  }
  if (system_event_collector_ != nullptr) {
    const auto values = system_event_collector_->diagnostics();
    report.event_polls = values.poll_count;
    // Native event families own this stable soak contract; opt-in process
    // lifecycle evidence has separate collector diagnostics.
    report.system_events_recorded =
        values.events_recorded - values.external_events_recorded;
    report.power_events_recorded = values.events_by_source.power;
    report.device_events_recorded = values.events_by_source.device;
    report.audio_events_recorded = values.events_by_source.audio;
    report.service_events_recorded =
        values.events_by_source.service_control_manager;
    report.defender_events_recorded = values.events_by_source.defender;
    report.windows_update_events_recorded =
        values.events_by_source.windows_update;
    report.application_events_recorded = values.events_by_source.application;
    report.network_events_recorded = values.events_by_source.network;
    report.graphics_events_recorded = values.events_by_source.graphics;
    report.storage_events_recorded = values.events_by_source.storage;
    report.native_events_dropped = values.native_events_dropped;
    report.event_provider_failures = values.provider_failures;
    report.event_provider_recoveries = values.provider_recoveries;
    report.event_worker_failures = values.worker_failures;
    report.automatic_event_requests = values.automatic_event_requests;
  }
#if BLACKBOX_STORAGE_ENABLED
  if (incident_writer_ != nullptr) {
    const auto values = incident_writer_->diagnostics();
    report.writer_attempts = values.attempts;
    report.writer_retry_attempts = values.retry_attempts;
    report.writer_retry_exhausted = values.retry_exhausted;
    report.writer_succeeded = values.succeeded;
    report.writer_failed = values.failed;
    report.writer_recoveries = values.recoveries;
    report.writer_cancelled = values.cancelled;
    report.recoverable_incident_available =
        values.recoverable_incident_available;
  }
  if (incident_archive_ != nullptr) {
    const auto count = incident_archive_->incident_count();
    const auto size = incident_archive_->database_size_bytes();
    const auto schema = incident_archive_->schema_version();
    if (count)
      report.archive_incidents = *count;
    if (size)
      report.archive_size_bytes = *size;
    if (schema)
      report.archive_schema_version = *schema;
    report.archive_healthy = count.has_value() && size.has_value() &&
                             schema.has_value() &&
                             *schema == storage::current_schema_version;
  }
#endif
  report.tray_available = final_shell_diagnostics_.tray_available;
  report.window_visible = final_shell_diagnostics_.window_visible;
  report.notifications_dropped = final_shell_diagnostics_.notifications_dropped;
  report.explorer_restarts = final_shell_diagnostics_.explorer_restarts;
  report.tray_readd_failures = final_shell_diagnostics_.tray_readd_failures;
  report.session_notifications_available =
      final_shell_diagnostics_.session_notifications_available;
  report.session_locks = final_shell_diagnostics_.session_locks;
  report.session_unlocks = final_shell_diagnostics_.session_unlocks;
  if (crash_diagnostics_ != nullptr) {
    final_crash_diagnostics_ = crash_diagnostics_->snapshot();
  }
  report.crash_diagnostics_armed = final_crash_diagnostics_.armed;
  report.previous_crash_dumps = final_crash_diagnostics_.completed_dumps;

  const auto result =
      write_wall_clock_report(diagnostic_options_.report_path, report);
  diagnostic_report_written_ = result.has_value();
  diagnostic_report_failed_ = !result.has_value();
  if (!result) {
    core::Logger::write(core::LogLevel::error, result.error().message);
  }
}

} // namespace blackbox::app
