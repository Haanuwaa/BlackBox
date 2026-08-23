#pragma once

#include "core/circular_recorder.hpp"
#include "core/clock.hpp"
#include "core/incident.hpp"
#include "telemetry/collection_timing.hpp"
#include "telemetry/automatic_incident_detector.hpp"
#include "telemetry/event_collector.hpp"
#include "telemetry/incident_snapshot_builder.hpp"
#include "telemetry/normalizer.hpp"
#include "telemetry/process_metadata_cache.hpp"
#include "telemetry/process_normalizer.hpp"
#include "telemetry/provider.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace blackbox::telemetry {

struct RecorderConfiguration {
    std::chrono::nanoseconds sample_interval{std::chrono::seconds{1}};
    std::chrono::nanoseconds history_duration{std::chrono::minutes{5}};
    std::chrono::nanoseconds late_tolerance{std::chrono::milliseconds{50}};
    std::chrono::nanoseconds metadata_interval{std::chrono::seconds{30}};
    std::chrono::nanoseconds incident_pre_window{std::chrono::seconds{120}};
    std::chrono::nanoseconds incident_post_window{std::chrono::seconds{30}};
    std::chrono::nanoseconds resume_gap_threshold{std::chrono::seconds{5}};
    bool collect_process_paths{true};
    friend constexpr bool operator==(const RecorderConfiguration&,
                                     const RecorderConfiguration&) = default;
};

enum class RecorderConfigurationError : std::uint8_t {
    interval_not_positive,
    history_not_positive,
    late_tolerance_negative,
    capacity_exceeded,
    metadata_interval_not_positive,
    incident_pre_window_negative,
    incident_post_window_negative,
    resume_gap_threshold_not_positive,
};

struct ValidatedRecorderConfiguration {
    RecorderConfiguration values{};
    std::size_t capacity{};
    std::size_t processes_per_frame_limit{};
    friend constexpr bool operator==(const ValidatedRecorderConfiguration&,
                                     const ValidatedRecorderConfiguration&) = default;
};

inline constexpr std::size_t maximum_history_samples = 86'400U;
inline constexpr std::size_t maximum_process_history_entries = 600'000U;
inline constexpr std::size_t incident_writer_queue_capacity = 2U;

[[nodiscard]] std::expected<ValidatedRecorderConfiguration, RecorderConfigurationError>
validate_recorder_configuration(RecorderConfiguration configuration) noexcept;

struct ScheduleAdvance {
    core::MonotonicTimePoint next_deadline{};
    std::chrono::nanoseconds deadline_overrun{};
    std::uint64_t dropped_ticks{};
    bool deadline_missed{};
    friend constexpr bool operator==(const ScheduleAdvance&, const ScheduleAdvance&) = default;
};

struct SchedulingDropEvent {
    core::MonotonicTimePoint observed_at{};
    std::chrono::nanoseconds deadline_overrun{};
    std::uint64_t collection_index{};
    std::uint64_t dropped_ticks{};
    friend constexpr bool operator==(const SchedulingDropEvent&,
                                     const SchedulingDropEvent&) = default;
};

inline constexpr std::size_t scheduling_drop_event_capacity = 256U;

struct SchedulingDropSnapshot {
    std::vector<SchedulingDropEvent> events{};
    std::uint64_t overflow{};
};

[[nodiscard]] ScheduleAdvance advance_schedule(
    core::MonotonicTimePoint scheduled_start,
    core::MonotonicTimePoint collection_finished,
    std::chrono::nanoseconds interval) noexcept;

struct ResumeGapDecision {
    bool detected{};
    std::chrono::nanoseconds gap{};
    std::uint64_t skipped_ticks{};
    friend constexpr bool operator==(const ResumeGapDecision&,
                                     const ResumeGapDecision&) = default;
};

[[nodiscard]] ResumeGapDecision detect_resume_gap(
    core::MonotonicTimePoint scheduled_start,
    core::MonotonicTimePoint actual_start,
    std::chrono::nanoseconds interval,
    std::chrono::nanoseconds threshold) noexcept;

struct CollectorDiagnostics {
    bool running{};
    RecorderConfiguration configuration{};
    std::uint64_t collection_count{};
    std::uint64_t partial_samples{};
    std::uint64_t failed_samples{};
    std::uint64_t dropped_samples{};
    std::uint64_t late_samples{};
    std::uint64_t deadline_misses{};
    std::uint64_t resume_events{};
    std::uint64_t resume_skipped_samples{};
    std::chrono::nanoseconds last_resume_gap{};
    std::uint64_t provider_recoveries{};
    std::uint64_t consecutive_provider_failures{};
    std::uint64_t worker_failures{};
    ProviderSampleStatus provider_status{ProviderSampleStatus::complete};
    CollectionTimingSummary collection_timing{};
    CollectionTimingSummary scheduling_jitter{};
    core::RecorderStatistics ring{};
    core::RecorderStatistics process_ring{};
    std::size_t active_processes{};
    std::size_t process_metadata_entries{};
    std::size_t process_metadata_capacity{};
    std::uint64_t process_metadata_evictions{};
    std::uint64_t process_inaccessible{};
    std::uint64_t processes_exited_during_sample{};
    std::uint64_t process_samples_truncated{};
    std::uint64_t process_lifecycle_observations{};
    std::uint64_t process_lifecycle_events_recorded{};
    RawProcessCollectionDiagnostics last_process_collection{};
    core::IncidentCaptureStatus incident_capture{};
    CollectionTimingSummary incident_snapshot_timing{};
    bool automatic_detection_enabled{};
    AutomaticDetectorDiagnostics automatic_detector{};
    std::uint64_t automatic_captures_started{};
    std::uint64_t automatic_captures_merged{};
    std::uint64_t automatic_capture_rejections{};
};

struct ActiveProcessSnapshot {
    ProcessFrame frame{};
    std::vector<ProcessInfo> metadata{};
};

class TelemetryCollector final : public core::IIncidentCaptureRequestSink {
public:
    TelemetryCollector(ITelemetryProvider& provider,
                       const core::IMonotonicClock& clock,
                       ValidatedRecorderConfiguration configuration,
                       IAutomaticIncidentDetector* automatic_detector = nullptr,
                       const ISystemEventHistory* event_history = nullptr,
                       ISystemEventSink* event_sink = nullptr);
    ~TelemetryCollector();

    TelemetryCollector(const TelemetryCollector&) = delete;
    TelemetryCollector& operator=(const TelemetryCollector&) = delete;
    TelemetryCollector(TelemetryCollector&&) = delete;
    TelemetryCollector& operator=(TelemetryCollector&&) = delete;

    void start();
    void stop() noexcept;
    void reconfigure(ValidatedRecorderConfiguration configuration);
    void set_automatic_detection_enabled(bool enabled) noexcept;
    void set_foreground_application_enabled(bool enabled) noexcept;
    void set_process_lifecycle_enabled(bool enabled) noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] core::RecorderSnapshot<SystemSample> snapshot(
        std::size_t maximum_samples) const;
    [[nodiscard]] CollectorDiagnostics diagnostics() const noexcept;
    [[nodiscard]] SchedulingDropSnapshot scheduling_drop_snapshot() const noexcept;
    [[nodiscard]] ActiveProcessSnapshot active_process_snapshot() const;
    [[nodiscard]] core::RecorderSnapshot<ProcessFrame> process_snapshot(
        std::size_t maximum_frames) const;
    [[nodiscard]] std::vector<ProcessInfo> process_metadata_snapshot() const;
    [[nodiscard]] core::IncidentCaptureRequestResult request_incident_capture() noexcept;
    [[nodiscard]] core::IncidentCaptureRequestResult request_incident_capture(
        core::MonotonicTimePoint event_time,
        core::IncidentCaptureTrigger trigger) noexcept override;
    [[nodiscard]] core::IncidentCaptureStatus incident_capture_status() const noexcept;
    [[nodiscard]] core::IIncidentWorkSource& incident_work_source() noexcept;
    [[nodiscard]] std::shared_ptr<const core::IncidentSnapshot>
    try_dequeue_incident() noexcept;

private:
    void run(std::stop_token stop_token);
    void set_running(bool value) noexcept;

    ITelemetryProvider& provider_;
    const core::IMonotonicClock& clock_;
    ValidatedRecorderConfiguration configuration_;
    core::CircularRecorder<SystemSample> recorder_;
    core::CircularRecorder<ProcessFrame> process_recorder_;
    SystemTelemetryNormalizer normalizer_{};
    ProcessTelemetryNormalizer process_normalizer_{};
    ProcessMetadataCache process_metadata_cache_;
    IAutomaticIncidentDetector* automatic_detector_{};
    const ISystemEventHistory* event_history_{};
    ISystemEventSink* event_sink_{};
    std::atomic<bool> automatic_detection_enabled_{};
    std::atomic<bool> foreground_application_enabled_{true};
    std::atomic<bool> process_lifecycle_enabled_{};
    std::atomic<bool> lifecycle_resynchronize_requested_{true};
    ProcessFrame normalized_process_frame_{};
    ProcessFrame bounded_process_frame_{};
    ProcessFrame active_process_frame_{};
    RawTelemetrySnapshot raw_snapshot_{};
    CollectionTimingWindow collection_timing_{};
    CollectionTimingWindow scheduling_jitter_{};
    CollectionTimingWindow incident_snapshot_timing_{};
    core::IncidentCaptureCoordinator incident_capture_{incident_writer_queue_capacity};

    mutable std::mutex process_mutex_{};

    mutable std::mutex diagnostics_mutex_{};
    CollectorDiagnostics diagnostics_{};
    std::unique_ptr<std::array<SchedulingDropEvent,
                               scheduling_drop_event_capacity>>
        scheduling_drop_events_{
            std::make_unique<std::array<SchedulingDropEvent,
                                        scheduling_drop_event_capacity>>()};
    std::size_t scheduling_drop_event_count_{};
    std::uint64_t scheduling_drop_event_overflow_{};
    bool has_logged_status_{};
    ProviderSampleStatus logged_status_{ProviderSampleStatus::complete};

    mutable std::mutex lifecycle_mutex_{};
    std::mutex wait_mutex_{};
    std::condition_variable_any wake_condition_{};
    std::jthread worker_{};
};

} // namespace blackbox::telemetry
