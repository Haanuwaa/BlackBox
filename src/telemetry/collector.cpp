#include "telemetry/collector.hpp"

#include "core/logger.hpp"
#include "telemetry/incident_snapshot_builder.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <string_view>

namespace blackbox::telemetry {
namespace {

[[nodiscard]] constexpr std::string_view provider_status_text(
    const ProviderSampleStatus status) noexcept {
    switch (status) {
    case ProviderSampleStatus::complete:
        return "Collecting";
    case ProviderSampleStatus::partial:
        return "Partial sample";
    case ProviderSampleStatus::temporarily_failed:
        return "Temporarily unavailable";
    }
    return "Unknown";
}

[[nodiscard]] SystemSample unavailable_sample(
    const core::MonotonicTimePoint observed_at) noexcept {
    const auto unavailable_ratio = MetricValue<Ratio>::unavailable(
        MetricStatus::temporarily_unavailable);
    const auto unavailable_bytes = MetricValue<ByteCount>::unavailable(
        MetricStatus::temporarily_unavailable);
    const auto unavailable_rate = MetricValue<BytesPerSecond>::unavailable(
        MetricStatus::temporarily_unavailable);

    SystemSample sample{};
    sample.observed_at = observed_at;
    sample.cpu_usage = unavailable_ratio;
    sample.memory_used = unavailable_bytes;
    sample.memory_total = unavailable_bytes;
    sample.memory_usage = unavailable_ratio;
    sample.disk_read_rate = unavailable_rate;
    sample.disk_write_rate = unavailable_rate;
    sample.network_receive_rate = unavailable_rate;
    sample.network_transmit_rate = unavailable_rate;
    return sample;
}

[[nodiscard]] std::chrono::nanoseconds nonnegative_duration(
    const core::MonotonicClock::duration duration) noexcept {
    if (duration <= core::MonotonicClock::duration::zero()) {
        return std::chrono::nanoseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
}

} // namespace

std::expected<ValidatedRecorderConfiguration, RecorderConfigurationError>
validate_recorder_configuration(const RecorderConfiguration configuration) noexcept {
    if (configuration.sample_interval <= std::chrono::nanoseconds::zero()) {
        return std::unexpected{RecorderConfigurationError::interval_not_positive};
    }
    if (configuration.history_duration <= std::chrono::nanoseconds::zero()) {
        return std::unexpected{RecorderConfigurationError::history_not_positive};
    }
    if (configuration.late_tolerance < std::chrono::nanoseconds::zero()) {
        return std::unexpected{RecorderConfigurationError::late_tolerance_negative};
    }
    if (configuration.metadata_interval <= std::chrono::nanoseconds::zero()) {
        return std::unexpected{RecorderConfigurationError::metadata_interval_not_positive};
    }
    if (configuration.incident_pre_window < std::chrono::nanoseconds::zero()) {
        return std::unexpected{RecorderConfigurationError::incident_pre_window_negative};
    }
    if (configuration.incident_post_window < std::chrono::nanoseconds::zero()) {
        return std::unexpected{RecorderConfigurationError::incident_post_window_negative};
    }
    if (configuration.resume_gap_threshold <= std::chrono::nanoseconds::zero()) {
        return std::unexpected{
            RecorderConfigurationError::resume_gap_threshold_not_positive};
    }

    const auto interval = configuration.sample_interval.count();
    const auto history = configuration.history_duration.count();
    const auto capacity = 1U + static_cast<std::uint64_t>((history - 1) / interval);
    if (capacity > maximum_history_samples) {
        return std::unexpected{RecorderConfigurationError::capacity_exceeded};
    }

    const auto process_limit = std::max<std::size_t>(
        1U, maximum_process_history_entries / static_cast<std::size_t>(capacity));
    return ValidatedRecorderConfiguration{configuration,
                                          static_cast<std::size_t>(capacity),
                                          process_limit};
}

ScheduleAdvance advance_schedule(
    const core::MonotonicTimePoint scheduled_start,
    const core::MonotonicTimePoint collection_finished,
    const std::chrono::nanoseconds interval) noexcept {
    ScheduleAdvance result{};
    result.next_deadline = scheduled_start + interval;
    result.deadline_missed = collection_finished > result.next_deadline;
    if (result.deadline_missed) {
        result.deadline_overrun = nonnegative_duration(
            collection_finished - result.next_deadline);
    }
    if (interval <= std::chrono::nanoseconds::zero() ||
        result.next_deadline > collection_finished) {
        return result;
    }

    const auto elapsed = collection_finished - result.next_deadline;
    const auto skipped = elapsed / interval + 1;
    result.dropped_ticks = static_cast<std::uint64_t>(skipped);
    result.next_deadline += interval * skipped;
    return result;
}

ResumeGapDecision detect_resume_gap(
    const core::MonotonicTimePoint scheduled_start,
    const core::MonotonicTimePoint actual_start,
    const std::chrono::nanoseconds interval,
    const std::chrono::nanoseconds threshold) noexcept {
    ResumeGapDecision result{};
    if (interval <= std::chrono::nanoseconds::zero() ||
        threshold <= std::chrono::nanoseconds::zero() ||
        actual_start <= scheduled_start) {
        return result;
    }
    result.gap = nonnegative_duration(actual_start - scheduled_start);
    if (result.gap < threshold) {
        return result;
    }
    result.detected = true;
    result.skipped_ticks = std::max<std::uint64_t>(
        1U, static_cast<std::uint64_t>(result.gap / interval));
    return result;
}

TelemetryCollector::TelemetryCollector(
    ITelemetryProvider& provider,
    const core::IMonotonicClock& clock,
    const ValidatedRecorderConfiguration configuration,
    IAutomaticIncidentDetector* const automatic_detector,
    const ISystemEventHistory* const event_history,
    ISystemEventSink* const event_sink)
    : provider_{provider},
      clock_{clock},
      configuration_{configuration},
      recorder_{configuration.capacity},
      process_recorder_{configuration.capacity},
      process_metadata_cache_{configuration.values.history_duration},
      automatic_detector_{automatic_detector},
      event_history_{event_history},
      event_sink_{event_sink},
      automatic_detection_enabled_{automatic_detector != nullptr} {
    diagnostics_.configuration = configuration.values;
    diagnostics_.ring = recorder_.statistics();
    diagnostics_.automatic_detection_enabled = automatic_detection_enabled_.load();
}

TelemetryCollector::~TelemetryCollector() {
    stop();
}

void TelemetryCollector::start() {
    const std::scoped_lock lock{lifecycle_mutex_};
    if (worker_.joinable()) {
        if (running()) {
            return;
        }
        worker_.join();
    }
    // A pause can span an arbitrary wall-clock interval. Re-establish rate
    // baselines on resume so the first observation cannot turn the pause gap
    // into a misleading throughput or CPU sample. Recorder history remains.
    normalizer_.reset();
    process_normalizer_.reset();
    if (automatic_detector_ != nullptr) {
        automatic_detector_->reset();
    }
    lifecycle_resynchronize_requested_.store(true);
    incident_capture_.start_accepting();
    set_running(true);
    try {
        worker_ = std::jthread{[this](const std::stop_token stop_token) {
            try {
                run(stop_token);
            } catch (...) {
                {
                    const std::scoped_lock diagnostics_lock{diagnostics_mutex_};
                    ++diagnostics_.worker_failures;
                }
                core::Logger::write(core::LogLevel::error, "telemetry",
                                    "Collector worker stopped after an unexpected failure");
            }
            set_running(false);
        }};
    } catch (...) {
        incident_capture_.stop_accepting();
        set_running(false);
        throw;
    }
}

void TelemetryCollector::stop() noexcept {
    const std::scoped_lock lock{lifecycle_mutex_};
    incident_capture_.stop_accepting();
    if (worker_.joinable()) {
        worker_.request_stop();
        wake_condition_.notify_all();
        try {
            worker_.join();
        } catch (...) {
            // Destruction and application shutdown must not propagate a thread
            // joining failure across the lifecycle boundary.
        }
    }
    set_running(false);
}

void TelemetryCollector::reconfigure(
    const ValidatedRecorderConfiguration configuration) {
    const bool restart = running();
    stop();

    configuration_ = configuration;
    recorder_.reconfigure(configuration.capacity);
    process_recorder_.reconfigure(configuration.capacity);
    normalizer_.reset();
    process_normalizer_.reset();
    lifecycle_resynchronize_requested_.store(true);
    if (automatic_detector_ != nullptr) {
        automatic_detector_->reset();
    }
    {
        const std::scoped_lock lock{process_mutex_};
        process_metadata_cache_.reset(configuration.values.history_duration);
        normalized_process_frame_.observed_at = core::MonotonicTimePoint{};
        normalized_process_frame_.processes.clear();
        bounded_process_frame_ = ProcessFrame{};
        active_process_frame_ = ProcessFrame{};
    }
    collection_timing_.reset();
    scheduling_jitter_.reset();
    incident_snapshot_timing_.reset();
    has_logged_status_ = false;

    {
        const std::scoped_lock lock{diagnostics_mutex_};
        diagnostics_ = CollectorDiagnostics{};
        diagnostics_.configuration = configuration.values;
        diagnostics_.ring = recorder_.statistics();
        diagnostics_.automatic_detection_enabled = automatic_detection_enabled_.load();
        *scheduling_drop_events_ = {};
        scheduling_drop_event_count_ = 0U;
        scheduling_drop_event_overflow_ = 0U;
    }

    if (restart) {
        start();
    }
}

void TelemetryCollector::set_automatic_detection_enabled(const bool enabled) noexcept {
    const bool effective = enabled && automatic_detector_ != nullptr;
    automatic_detection_enabled_.store(effective);
    const std::scoped_lock lock{diagnostics_mutex_};
    diagnostics_.automatic_detection_enabled = effective;
}

void TelemetryCollector::set_foreground_application_enabled(const bool enabled) noexcept {
    foreground_application_enabled_.store(enabled);
}

void TelemetryCollector::set_process_lifecycle_enabled(const bool enabled) noexcept {
    process_lifecycle_enabled_.store(enabled);
    lifecycle_resynchronize_requested_.store(true);
}

bool TelemetryCollector::running() const noexcept {
    const std::scoped_lock lock{diagnostics_mutex_};
    return diagnostics_.running;
}

core::RecorderSnapshot<SystemSample> TelemetryCollector::snapshot(
    const std::size_t maximum_samples) const {
    return recorder_.snapshot(maximum_samples);
}

CollectorDiagnostics TelemetryCollector::diagnostics() const noexcept {
    CollectorDiagnostics result{};
    {
        const std::scoped_lock lock{diagnostics_mutex_};
        result = diagnostics_;
    }
    result.ring = recorder_.statistics();
    result.process_ring = process_recorder_.statistics();
    result.incident_capture = incident_capture_.status();
    {
        const std::scoped_lock lock{process_mutex_};
        result.active_processes = active_process_frame_.processes.size();
        result.process_metadata_entries = process_metadata_cache_.size();
        result.process_metadata_capacity = process_metadata_cache_.maximum_entries();
        result.process_metadata_evictions = process_metadata_cache_.evictions();
    }
    return result;
}

SchedulingDropSnapshot TelemetryCollector::scheduling_drop_snapshot() const noexcept {
    const std::scoped_lock lock{diagnostics_mutex_};
    SchedulingDropSnapshot snapshot{};
    try {
        snapshot.events.reserve(scheduling_drop_event_count_);
        snapshot.events.insert(snapshot.events.end(), scheduling_drop_events_->begin(),
                               scheduling_drop_events_->begin() +
                                   static_cast<std::ptrdiff_t>(
                                       scheduling_drop_event_count_));
        snapshot.overflow = scheduling_drop_event_overflow_;
    } catch (...) {
        snapshot.events.clear();
        snapshot.overflow = scheduling_drop_event_overflow_ +
                            static_cast<std::uint64_t>(
                                scheduling_drop_event_count_);
    }
    return snapshot;
}

ActiveProcessSnapshot TelemetryCollector::active_process_snapshot() const {
    const std::scoped_lock lock{process_mutex_};
    ActiveProcessSnapshot result{};
    result.frame = active_process_frame_;
    result.metadata = process_metadata_cache_.active_snapshot(
        result.frame.processes);
    return result;
}

core::RecorderSnapshot<ProcessFrame> TelemetryCollector::process_snapshot(
    const std::size_t maximum_frames) const {
    return process_recorder_.snapshot(maximum_frames);
}

std::vector<ProcessInfo> TelemetryCollector::process_metadata_snapshot() const {
    const std::scoped_lock lock{process_mutex_};
    return process_metadata_cache_.snapshot();
}

core::IncidentCaptureRequestResult TelemetryCollector::request_incident_capture() noexcept {
    const std::scoped_lock lock{lifecycle_mutex_};
    return incident_capture_.request(
        clock_.now(), configuration_.values.incident_pre_window,
        configuration_.values.incident_post_window);
}

core::IncidentCaptureRequestResult TelemetryCollector::request_incident_capture(
    const core::MonotonicTimePoint event_time,
    const core::IncidentCaptureTrigger trigger) noexcept {
    return incident_capture_.request(
        event_time, configuration_.values.incident_pre_window,
        configuration_.values.incident_post_window, trigger);
}

core::IncidentCaptureStatus TelemetryCollector::incident_capture_status() const noexcept {
    return incident_capture_.status();
}

core::IIncidentWorkSource& TelemetryCollector::incident_work_source() noexcept {
    return incident_capture_;
}

std::shared_ptr<const core::IncidentSnapshot>
TelemetryCollector::try_dequeue_incident() noexcept {
    return incident_capture_.try_pop();
}

void TelemetryCollector::run(const std::stop_token stop_token) {
    const bool sampling_thread_prepared = provider_.prepare_sampling_thread();
    {
        const std::scoped_lock lock{diagnostics_mutex_};
        diagnostics_.sampling_thread_prepared = sampling_thread_prepared;
    }
    if (!sampling_thread_prepared) {
        core::Logger::write(core::LogLevel::warning, "telemetry",
                            "Sampling thread preparation was unavailable");
    }
    auto scheduled_start = clock_.now();
    auto next_metadata_collection = scheduled_start;

    while (!stop_token.stop_requested()) {
        {
            std::unique_lock lock{wait_mutex_};
            static_cast<void>(wake_condition_.wait_until(
                lock, stop_token, scheduled_start, [] { return false; }));
        }
        if (stop_token.stop_requested()) {
            break;
        }

        const auto actual_start = clock_.now();
        const auto resume = detect_resume_gap(
            scheduled_start, actual_start, configuration_.values.sample_interval,
            configuration_.values.resume_gap_threshold);
        if (resume.detected) {
            normalizer_.reset();
            process_normalizer_.reset();
            if (automatic_detector_ != nullptr) {
                automatic_detector_->reset();
            }
            lifecycle_resynchronize_requested_.store(true);
            scheduled_start = actual_start;
            next_metadata_collection = actual_start;
            {
                const std::scoped_lock lock{diagnostics_mutex_};
                ++diagnostics_.resume_events;
                diagnostics_.resume_skipped_samples += resume.skipped_ticks;
                diagnostics_.last_resume_gap = resume.gap;
            }
            core::Logger::write(core::LogLevel::info, "telemetry",
                                "Resume gap detected; telemetry baselines reset");
        }
        const auto jitter = nonnegative_duration(actual_start - scheduled_start);
        scheduling_jitter_.record(jitter);

        ProviderSampleStatus status = ProviderSampleStatus::temporarily_failed;
        SystemSample normalized{};
        normalized_process_frame_ = ProcessFrame{};
        try {
            auto tiers = SamplingTier::fast | SamplingTier::normal;
            if (configuration_.values.collect_process_paths &&
                actual_start >= next_metadata_collection) {
                tiers = tiers | SamplingTier::slow;
                do {
                    next_metadata_collection += configuration_.values.metadata_interval;
                } while (next_metadata_collection <= actual_start);
            }
            auto request = SamplingRequest{tiers};
            request.collect_foreground_application =
                foreground_application_enabled_.load();
            const auto result = provider_.sample(request, raw_snapshot_);
            status = result.status;
            normalized = normalizer_.normalize(raw_snapshot_);
            normalized_process_frame_.observed_at = raw_snapshot_.observed_at;
            process_normalizer_.normalize(raw_snapshot_,
                                          normalized_process_frame_.processes);
            const bool resynchronize =
                lifecycle_resynchronize_requested_.exchange(false);
            std::uint64_t lifecycle_observations{};
            std::uint64_t lifecycle_recorded{};
            if (status != ProviderSampleStatus::temporarily_failed) {
                lifecycle_observations = raw_snapshot_.process_lifecycle_events.size();
                if (!resynchronize && process_lifecycle_enabled_.load() &&
                    event_sink_ != nullptr) {
                    for (const auto& lifecycle : raw_snapshot_.process_lifecycle_events) {
                        core::SystemEvent event{};
                        event.observed_at = raw_snapshot_.observed_at;
                        event.source = core::SystemEventSource::process;
                        event.kind = lifecycle.kind == RawProcessLifecycleKind::started
                                         ? core::SystemEventKind::process_started
                                         : core::SystemEventKind::process_exited;
                        event.level = core::SystemEventLevel::informational;
                        event.has_process_identity = true;
                        event.process_pid = lifecycle.identity.pid.value;
                        event.process_creation_token = lifecycle.identity.creation_token;
                        if (event_sink_->record_external_event(event)) {
                            ++lifecycle_recorded;
                        }
                    }
                }
            } else {
                lifecycle_resynchronize_requested_.store(true);
            }
            if (lifecycle_observations != 0U || lifecycle_recorded != 0U) {
                const std::scoped_lock lock{diagnostics_mutex_};
                diagnostics_.process_lifecycle_observations += lifecycle_observations;
                diagnostics_.process_lifecycle_events_recorded += lifecycle_recorded;
            }
            if (status == ProviderSampleStatus::temporarily_failed) {
                normalizer_.reset();
                process_normalizer_.reset();
                if (automatic_detector_ != nullptr) {
                    automatic_detector_->reset();
                }
            }
        } catch (const std::exception&) {
            normalizer_.reset();
            process_normalizer_.reset();
            if (automatic_detector_ != nullptr) {
                automatic_detector_->reset();
            }
            normalized = unavailable_sample(clock_.now());
            normalized_process_frame_.observed_at = normalized.observed_at;
            normalized_process_frame_.processes.clear();
            raw_snapshot_.reset(normalized.observed_at,
                                SamplingTier::fast | SamplingTier::normal);
            lifecycle_resynchronize_requested_.store(true);
        } catch (...) {
            normalizer_.reset();
            process_normalizer_.reset();
            if (automatic_detector_ != nullptr) {
                automatic_detector_->reset();
            }
            normalized = unavailable_sample(clock_.now());
            normalized_process_frame_.observed_at = normalized.observed_at;
            normalized_process_frame_.processes.clear();
            raw_snapshot_.reset(normalized.observed_at,
                                SamplingTier::fast | SamplingTier::normal);
            lifecycle_resynchronize_requested_.store(true);
        }

        std::optional<core::IncidentCaptureRequestResult> automatic_capture_result;
        if (automatic_detector_ != nullptr && automatic_detection_enabled_.load()) {
            if (const auto trigger = automatic_detector_->observe(normalized)) {
                automatic_capture_result = incident_capture_.request(
                    normalized.observed_at,
                    configuration_.values.incident_pre_window,
                    configuration_.values.incident_post_window, *trigger);
            }
        }
        recorder_.append(std::move(normalized));
        const auto recorded_processes = std::min(
            normalized_process_frame_.processes.size(),
            configuration_.processes_per_frame_limit);
        if (recorded_processes == normalized_process_frame_.processes.size()) {
            process_recorder_.append(normalized_process_frame_);
        } else {
            bounded_process_frame_.observed_at = normalized_process_frame_.observed_at;
            bounded_process_frame_.processes.assign(
                normalized_process_frame_.processes.begin(),
                normalized_process_frame_.processes.begin() +
                    static_cast<std::ptrdiff_t>(recorded_processes));
            process_recorder_.append(bounded_process_frame_);
        }
        {
            const std::scoped_lock lock{process_mutex_};
            active_process_frame_ = normalized_process_frame_;
            process_metadata_cache_.update(
                raw_snapshot_.process_metadata,
                active_process_frame_.processes,
                active_process_frame_.observed_at);
        }

        if (const auto window = incident_capture_.try_begin_snapshot(
                normalized_process_frame_.observed_at)) {
            const auto snapshot_started = clock_.now();
            try {
                const auto elapsed = normalized_process_frame_.observed_at -
                                     window->requested_start;
                const auto interval = configuration_.values.sample_interval;
                const auto elapsed_ticks = elapsed > core::MonotonicClock::duration::zero()
                                               ? elapsed / interval
                                               : 0;
                const auto requested_frames = std::min<std::size_t>(
                    configuration_.capacity,
                    static_cast<std::size_t>(elapsed_ticks) + 2U);
                auto system_history = recorder_.snapshot(requested_frames);
                auto process_history = process_recorder_.snapshot(requested_frames);
                auto metadata = process_metadata_snapshot();
                auto event_history = event_history_ != nullptr
                                         ? std::optional{event_history_->snapshot(
                                               maximum_event_ring_capacity)}
                                         : std::optional<core::RecorderSnapshot<
                                               core::SystemEvent>>{};
                incident_capture_.finish_snapshot(build_incident_snapshot(
                    *window, normalized_process_frame_.observed_at, system_history,
                    process_history, metadata,
                    event_history.has_value() ? &*event_history : nullptr));
            } catch (...) {
                incident_capture_.finish_snapshot({});
            }
            incident_snapshot_timing_.record(nonnegative_duration(
                clock_.now() - snapshot_started));
        }
        const auto finished = clock_.now();
        collection_timing_.record(nonnegative_duration(finished - actual_start));
        const auto schedule = advance_schedule(
            scheduled_start, finished, configuration_.values.sample_interval);

        {
            const std::scoped_lock lock{diagnostics_mutex_};
            ++diagnostics_.collection_count;
            diagnostics_.provider_status = status;
            diagnostics_.dropped_samples += schedule.dropped_ticks;
            if (schedule.dropped_ticks != 0U) {
                if (scheduling_drop_event_count_ <
                    scheduling_drop_events_->size()) {
                    (*scheduling_drop_events_)[scheduling_drop_event_count_++] =
                        SchedulingDropEvent{finished, schedule.deadline_overrun,
                                            diagnostics_.collection_count,
                                            schedule.dropped_ticks};
                } else {
                    ++scheduling_drop_event_overflow_;
                }
            }
            if (status == ProviderSampleStatus::partial) {
                ++diagnostics_.partial_samples;
            } else if (status == ProviderSampleStatus::temporarily_failed) {
                ++diagnostics_.failed_samples;
                ++diagnostics_.consecutive_provider_failures;
            } else if (diagnostics_.consecutive_provider_failures != 0U) {
                ++diagnostics_.provider_recoveries;
                diagnostics_.consecutive_provider_failures = 0U;
            }
            if (jitter > configuration_.values.late_tolerance) {
                ++diagnostics_.late_samples;
            }
            if (schedule.deadline_missed) {
                ++diagnostics_.deadline_misses;
            }
            diagnostics_.collection_timing = collection_timing_.summary();
            diagnostics_.scheduling_jitter = scheduling_jitter_.summary();
            diagnostics_.incident_snapshot_timing = incident_snapshot_timing_.summary();
            diagnostics_.last_process_collection = raw_snapshot_.process_diagnostics;
            diagnostics_.process_inaccessible +=
                raw_snapshot_.process_diagnostics.inaccessible;
            diagnostics_.processes_exited_during_sample +=
                raw_snapshot_.process_diagnostics.exited_during_sample;
            diagnostics_.process_samples_truncated +=
                normalized_process_frame_.processes.size() - recorded_processes;
            if (automatic_detector_ != nullptr) {
                diagnostics_.automatic_detection_enabled =
                    automatic_detection_enabled_.load();
                diagnostics_.automatic_detector = automatic_detector_->diagnostics();
            }
            if (automatic_capture_result.has_value()) {
                switch (*automatic_capture_result) {
                case core::IncidentCaptureRequestResult::started:
                    ++diagnostics_.automatic_captures_started;
                    break;
                case core::IncidentCaptureRequestResult::merged:
                    ++diagnostics_.automatic_captures_merged;
                    break;
                case core::IncidentCaptureRequestResult::queue_full:
                case core::IncidentCaptureRequestResult::stopped:
                    ++diagnostics_.automatic_capture_rejections;
                    break;
                }
            }
        }

        if (!has_logged_status_ || logged_status_ != status) {
            const auto level = status == ProviderSampleStatus::complete
                                   ? core::LogLevel::info
                                   : core::LogLevel::warning;
            core::Logger::write(level, "telemetry", provider_status_text(status));
            logged_status_ = status;
            has_logged_status_ = true;
        }

        scheduled_start = schedule.next_deadline;
    }
}

void TelemetryCollector::set_running(const bool value) noexcept {
    const std::scoped_lock lock{diagnostics_mutex_};
    diagnostics_.running = value;
}

} // namespace blackbox::telemetry
