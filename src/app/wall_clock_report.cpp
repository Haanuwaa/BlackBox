#include "app/wall_clock_report.hpp"

#include <fstream>
#include <exception>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace blackbox::app {
namespace {

[[nodiscard]] WallClockReportError error(const WallClockReportErrorCode code,
                                         std::string message) {
    return WallClockReportError{code, std::move(message)};
}

[[nodiscard]] bool safe_identifier(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 64U) return false;
    for (const unsigned char character : value) {
        const bool allowed = (character >= 'a' && character <= 'z') ||
                             (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') ||
                             character == '.' || character == '-' || character == '_';
        if (!allowed) return false;
    }
    return true;
}

template <typename Value>
void append(std::ostringstream& output, const std::string_view name,
            const Value value) {
    output << name << '=' << value << '\n';
}

void append(std::ostringstream& output, const std::string_view name,
            const bool value) {
    output << name << '=' << (value ? 1 : 0) << '\n';
}

} // namespace

std::expected<void, WallClockReportError> write_wall_clock_report(
    const std::filesystem::path& destination,
    const WallClockReport& report) noexcept {
    try {
        if (destination.empty() || !destination.is_absolute() ||
            destination.filename().empty() || destination.extension() != ".ini" ||
            !safe_identifier(report.application_version) ||
            !safe_identifier(report.platform) ||
            !safe_identifier(report.source_revision) ||
            report.requested_runtime_seconds == 0U ||
            report.requested_runtime_seconds > 7U * 24U * 60U * 60U ||
            report.capture_interval_seconds > 24U * 60U * 60U) {
            return std::unexpected{error(
                WallClockReportErrorCode::invalid_report,
                "wall-clock report requires bounded values and an absolute .ini destination")};
        }
        auto staging = destination;
        staging += ".partial";
        std::error_code issue;
        if (std::filesystem::exists(destination, issue) || issue ||
            std::filesystem::exists(staging, issue) || issue) {
            return std::unexpected{error(WallClockReportErrorCode::destination_exists,
                                         "wall-clock report destination is occupied")};
        }
        const auto parent = destination.parent_path();
        if (parent.empty() || !std::filesystem::is_directory(parent, issue) || issue) {
            return std::unexpected{error(WallClockReportErrorCode::invalid_report,
                                         "wall-clock report parent must already exist")};
        }

        std::ostringstream output;
        append(output, "format", wall_clock_report_format_version);
        append(output, "application_version", report.application_version);
        append(output, "platform", report.platform);
        append(output, "source_revision", report.source_revision);
        append(output, "completed", report.completed);
        append(output, "requested_runtime_seconds", report.requested_runtime_seconds);
        append(output, "capture_interval_seconds", report.capture_interval_seconds);
        append(output, "collections", report.collections);
        append(output, "partial_samples", report.partial_samples);
        append(output, "failed_samples", report.failed_samples);
        append(output, "dropped_samples", report.dropped_samples);
        append(output, "late_samples", report.late_samples);
        append(output, "deadline_misses", report.deadline_misses);
        append(output, "resume_events", report.resume_events);
        append(output, "resume_skipped_samples", report.resume_skipped_samples);
        append(output, "provider_recoveries", report.provider_recoveries);
        append(output, "collector_worker_failures", report.collector_worker_failures);
        append(output, "collection_p99_nanoseconds", report.collection_p99_nanoseconds);
        append(output, "collection_maximum_nanoseconds", report.collection_maximum_nanoseconds);
        append(output, "jitter_p99_nanoseconds", report.jitter_p99_nanoseconds);
        append(output, "jitter_maximum_nanoseconds", report.jitter_maximum_nanoseconds);
        append(output, "ring_capacity", report.ring_capacity);
        append(output, "ring_size", report.ring_size);
        append(output, "ring_total_appends", report.ring_total_appends);
        append(output, "ring_overwritten_samples", report.ring_overwritten_samples);
        append(output, "ring_discarded_samples", report.ring_discarded_samples);
        append(output, "process_ring_capacity", report.process_ring_capacity);
        append(output, "process_ring_size", report.process_ring_size);
        append(output, "process_ring_total_appends", report.process_ring_total_appends);
        append(output, "process_ring_overwritten_samples", report.process_ring_overwritten_samples);
        append(output, "process_ring_discarded_samples", report.process_ring_discarded_samples);
        append(output, "captures_started", report.captures_started);
        append(output, "capture_requests_merged", report.capture_requests_merged);
        append(output, "incidents_completed", report.incidents_completed);
        append(output, "capture_queue_rejections", report.capture_queue_rejections);
        append(output, "snapshot_failures", report.snapshot_failures);
        append(output, "captures_cancelled", report.captures_cancelled);
        append(output, "automatic_detection_enabled",
               report.automatic_detection_enabled);
        append(output, "automatic_detector_triggers",
               report.automatic_detector_triggers);
        append(output, "automatic_captures_started",
               report.automatic_captures_started);
        append(output, "automatic_event_requests", report.automatic_event_requests);
        append(output, "event_polls", report.event_polls);
        append(output, "system_events_recorded", report.system_events_recorded);
        append(output, "power_events_recorded", report.power_events_recorded);
        append(output, "device_events_recorded", report.device_events_recorded);
        append(output, "audio_events_recorded", report.audio_events_recorded);
        append(output, "service_events_recorded", report.service_events_recorded);
        append(output, "defender_events_recorded", report.defender_events_recorded);
        append(output, "windows_update_events_recorded",
               report.windows_update_events_recorded);
        append(output, "application_events_recorded", report.application_events_recorded);
        append(output, "network_events_recorded", report.network_events_recorded);
        append(output, "graphics_events_recorded", report.graphics_events_recorded);
        append(output, "storage_events_recorded", report.storage_events_recorded);
        append(output, "native_events_dropped", report.native_events_dropped);
        append(output, "event_provider_failures", report.event_provider_failures);
        append(output, "event_provider_recoveries", report.event_provider_recoveries);
        append(output, "event_worker_failures", report.event_worker_failures);
        append(output, "writer_attempts", report.writer_attempts);
        append(output, "writer_retry_attempts", report.writer_retry_attempts);
        append(output, "writer_retry_exhausted", report.writer_retry_exhausted);
        append(output, "writer_succeeded", report.writer_succeeded);
        append(output, "writer_failed", report.writer_failed);
        append(output, "writer_recoveries", report.writer_recoveries);
        append(output, "writer_cancelled", report.writer_cancelled);
        append(output, "recoverable_incident_available", report.recoverable_incident_available);
        append(output, "archive_healthy", report.archive_healthy);
        append(output, "archive_incidents", report.archive_incidents);
        append(output, "archive_size_bytes", report.archive_size_bytes);
        append(output, "archive_schema_version", report.archive_schema_version);
        append(output, "tray_available", report.tray_available);
        append(output, "notifications_dropped", report.notifications_dropped);
        append(output, "explorer_restarts", report.explorer_restarts);
        append(output, "tray_readd_failures", report.tray_readd_failures);
        append(output, "session_notifications_available",
               report.session_notifications_available);
        append(output, "session_locks", report.session_locks);
        append(output, "session_unlocks", report.session_unlocks);
        append(output, "crash_diagnostics_armed", report.crash_diagnostics_armed);
        append(output, "previous_crash_dumps", report.previous_crash_dumps);

        {
            std::ofstream stream{staging, std::ios::binary | std::ios::trunc};
            const auto contents = output.str();
            stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            stream.flush();
            if (!stream) {
                return std::unexpected{error(WallClockReportErrorCode::cannot_write,
                                             "cannot write wall-clock report staging file")};
            }
        }
        std::filesystem::rename(staging, destination, issue);
        if (issue) {
            return std::unexpected{error(WallClockReportErrorCode::cannot_write,
                                         "cannot publish wall-clock report atomically")};
        }
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected{error(WallClockReportErrorCode::cannot_write,
                                     exception.what())};
    } catch (...) {
        return std::unexpected{error(WallClockReportErrorCode::cannot_write,
                                     "unknown wall-clock report failure")};
    }
}

} // namespace blackbox::app
