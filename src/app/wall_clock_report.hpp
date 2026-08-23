#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace blackbox::app {

inline constexpr std::uint32_t wall_clock_report_format_version = 1U;
inline constexpr std::size_t wall_clock_scheduling_drop_event_capacity = 256U;

struct WallClockSchedulingDropEvent {
    std::uint64_t collection_index{};
    std::uint64_t utc_unix_nanoseconds{};
    std::uint64_t deadline_overrun_nanoseconds{};
    std::uint64_t dropped_ticks{};
};

enum class WallClockReportErrorCode : std::uint8_t {
    invalid_report,
    destination_exists,
    cannot_write,
};

struct WallClockReportError {
    WallClockReportErrorCode code{WallClockReportErrorCode::invalid_report};
    std::string message{};
};

// Privacy-safe, path-free, direct-v1 evidence emitted only by an explicit
// bounded diagnostic run. Counters are flattened so the report format does not
// become a runtime dependency of telemetry, storage, or platform layers.
struct WallClockReport {
    std::string application_version{};
    std::string platform{};
    std::string video_driver{};
    std::string source_revision{};
    bool completed{};
    std::uint64_t requested_runtime_seconds{};
    std::uint64_t capture_interval_seconds{};

    std::uint64_t collections{};
    bool sampling_thread_prepared{};
    std::uint64_t partial_samples{};
    std::uint64_t failed_samples{};
    std::uint64_t dropped_samples{};
    std::uint64_t late_samples{};
    std::uint64_t deadline_misses{};
    std::vector<WallClockSchedulingDropEvent> scheduling_drop_events{};
    std::uint64_t scheduling_drop_event_overflow{};
    std::uint64_t resume_events{};
    std::uint64_t resume_skipped_samples{};
    std::uint64_t provider_recoveries{};
    std::uint64_t collector_worker_failures{};
    std::uint64_t collection_p99_nanoseconds{};
    std::uint64_t collection_maximum_nanoseconds{};
    std::uint64_t jitter_p99_nanoseconds{};
    std::uint64_t jitter_maximum_nanoseconds{};

    std::uint64_t ring_capacity{};
    std::uint64_t ring_size{};
    std::uint64_t ring_total_appends{};
    std::uint64_t ring_overwritten_samples{};
    std::uint64_t ring_discarded_samples{};
    std::uint64_t process_ring_capacity{};
    std::uint64_t process_ring_size{};
    std::uint64_t process_ring_total_appends{};
    std::uint64_t process_ring_overwritten_samples{};
    std::uint64_t process_ring_discarded_samples{};

    std::uint64_t captures_started{};
    std::uint64_t capture_requests_merged{};
    std::uint64_t incidents_completed{};
    std::uint64_t capture_queue_rejections{};
    std::uint64_t snapshot_failures{};
    std::uint64_t captures_cancelled{};
    bool automatic_detection_enabled{};
    std::uint64_t automatic_detector_triggers{};
    std::uint64_t automatic_captures_started{};
    std::uint64_t automatic_event_requests{};

    std::uint64_t event_polls{};
    std::uint64_t system_events_recorded{};
    std::uint64_t power_events_recorded{};
    std::uint64_t device_events_recorded{};
    std::uint64_t audio_events_recorded{};
    std::uint64_t service_events_recorded{};
    std::uint64_t defender_events_recorded{};
    std::uint64_t windows_update_events_recorded{};
    std::uint64_t application_events_recorded{};
    std::uint64_t network_events_recorded{};
    std::uint64_t graphics_events_recorded{};
    std::uint64_t storage_events_recorded{};
    std::uint64_t native_events_dropped{};
    std::uint64_t event_provider_failures{};
    std::uint64_t event_provider_recoveries{};
    std::uint64_t event_worker_failures{};

    std::uint64_t writer_attempts{};
    std::uint64_t writer_retry_attempts{};
    std::uint64_t writer_retry_exhausted{};
    std::uint64_t writer_succeeded{};
    std::uint64_t writer_failed{};
    std::uint64_t writer_recoveries{};
    std::uint64_t writer_cancelled{};
    bool recoverable_incident_available{};
    bool archive_healthy{};
    std::uint64_t archive_incidents{};
    std::uint64_t archive_size_bytes{};
    std::int64_t archive_schema_version{};

    bool tray_available{};
    bool window_visible{true};
    std::uint64_t notifications_dropped{};
    std::uint64_t explorer_restarts{};
    std::uint64_t tray_readd_failures{};
    bool session_notifications_available{};
    std::uint64_t session_locks{};
    std::uint64_t session_unlocks{};
    bool crash_diagnostics_armed{};
    std::uint64_t previous_crash_dumps{};
};

[[nodiscard]] std::expected<void, WallClockReportError> write_wall_clock_report(
    const std::filesystem::path& destination,
    const WallClockReport& report) noexcept;

} // namespace blackbox::app
