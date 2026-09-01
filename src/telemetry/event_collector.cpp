#include "telemetry/event_collector.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace blackbox::telemetry {
namespace {

void record_source(EventSourceCounts& counts, const core::SystemEventSource source) noexcept {
    switch (source) {
    case core::SystemEventSource::power:
        ++counts.power;
        break;
    case core::SystemEventSource::device:
        ++counts.device;
        break;
    case core::SystemEventSource::audio:
        ++counts.audio;
        break;
    case core::SystemEventSource::service_manager:
        ++counts.service_manager;
        break;
    case core::SystemEventSource::security:
        ++counts.security;
        break;
    case core::SystemEventSource::update:
        ++counts.update;
        break;
    case core::SystemEventSource::application:
        ++counts.application;
        break;
    case core::SystemEventSource::network:
        ++counts.network;
        break;
    case core::SystemEventSource::graphics:
        ++counts.graphics;
        break;
    case core::SystemEventSource::storage:
        ++counts.storage;
        break;
    case core::SystemEventSource::process:
        ++counts.process;
        break;
    }
}

struct AutomaticEventTrigger {
    core::AutomaticIncidentResource resource{core::AutomaticIncidentResource::none};
    core::AutomaticIncidentSignal signal{core::AutomaticIncidentSignal::throughput_or_utilization};
};

[[nodiscard]] constexpr std::optional<AutomaticEventTrigger>
automatic_trigger(const core::SystemEventKind kind) noexcept {
    switch (kind) {
    case core::SystemEventKind::application_crash:
        return AutomaticEventTrigger{core::AutomaticIncidentResource::none,
                                     core::AutomaticIncidentSignal::application_crash};
    case core::SystemEventKind::application_hang:
        return AutomaticEventTrigger{core::AutomaticIncidentResource::none,
                                     core::AutomaticIncidentSignal::application_hang};
    case core::SystemEventKind::display_driver_recovery:
        return AutomaticEventTrigger{core::AutomaticIncidentResource::none,
                                     core::AutomaticIncidentSignal::display_driver_recovery};
    case core::SystemEventKind::storage_io_retry:
        return AutomaticEventTrigger{core::AutomaticIncidentResource::disk,
                                     core::AutomaticIncidentSignal::storage_io_retry};
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr bool resets_sampling_cadence(const core::SystemEvent& event) noexcept {
    return event.source == core::SystemEventSource::power &&
           (event.kind == core::SystemEventKind::suspend ||
            event.kind == core::SystemEventKind::resume_automatic ||
            event.kind == core::SystemEventKind::resume_user);
}

} // namespace

SystemEventCollector::SystemEventCollector(ISystemEventProvider& provider,
                                           const core::IMonotonicClock& clock,
                                           EventCollectorConfiguration configuration)
    : provider_{provider}, clock_{clock}, configuration_{configuration},
      recorder_{configuration.ring_capacity},
      batch_{std::make_unique<std::array<core::SystemEvent, maximum_events_per_poll>>()} {
    validate(configuration_);
    diagnostics_.configuration = configuration_;
    diagnostics_.capabilities = provider_.capabilities();
    diagnostics_.ring = recorder_.statistics();
}

SystemEventCollector::~SystemEventCollector() { stop(); }

void SystemEventCollector::validate(const EventCollectorConfiguration& configuration) {
    if (configuration.poll_interval <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument{"event poll interval must be positive"};
    }
    if (configuration.ring_capacity == 0U ||
        configuration.ring_capacity > maximum_event_ring_capacity) {
        throw std::invalid_argument{"event ring capacity is outside its bound"};
    }
}

void SystemEventCollector::start() {
    const std::scoped_lock lock{lifecycle_mutex_};
    if (worker_.joinable()) {
        return;
    }
    worker_ = std::jthread{[this](const std::stop_token token) { run(token); }};
}

void SystemEventCollector::stop() noexcept {
    std::jthread worker;
    {
        const std::scoped_lock lock{lifecycle_mutex_};
        if (!worker_.joinable()) {
            return;
        }
        worker_.request_stop();
        wake_condition_.notify_all();
        worker = std::move(worker_);
    }
    if (worker.joinable()) {
        worker.join();
    }
}

void SystemEventCollector::reconfigure(EventCollectorConfiguration configuration) {
    validate(configuration);
    const bool was_running = running();
    stop();
    {
        const std::scoped_lock lock{lifecycle_mutex_};
        configuration_ = configuration;
        recorder_.reconfigure(configuration_.ring_capacity);
        const std::scoped_lock diagnostics_lock{diagnostics_mutex_};
        diagnostics_ = EventCollectorDiagnostics{};
        diagnostics_.configuration = configuration_;
        diagnostics_.capabilities = provider_.capabilities();
        diagnostics_.ring = recorder_.statistics();
    }
    if (was_running) {
        start();
    }
}

void SystemEventCollector::set_incident_capture_sink(
    core::IIncidentCaptureRequestSink* const sink) noexcept {
    const std::scoped_lock lock{lifecycle_mutex_};
    if (!worker_.joinable()) incident_capture_sink_ = sink;
}

bool SystemEventCollector::running() const noexcept {
    const std::scoped_lock lock{diagnostics_mutex_};
    return diagnostics_.running;
}

core::RecorderSnapshot<core::SystemEvent>
SystemEventCollector::snapshot(const std::size_t maximum_events) const {
    return recorder_.snapshot(maximum_events);
}

bool SystemEventCollector::record_external_event(const core::SystemEvent& event) noexcept {
    const bool lifecycle_kind = event.kind == core::SystemEventKind::process_started ||
                                event.kind == core::SystemEventKind::process_exited;
    if (event.source != core::SystemEventSource::process || !lifecycle_kind ||
        !event.has_process_identity || event.process_pid == 0U ||
        event.process_creation_token == 0U) {
        return false;
    }
    try {
        recorder_.append(event);
        const std::scoped_lock lock{diagnostics_mutex_};
        ++diagnostics_.events_recorded;
        ++diagnostics_.external_events_recorded;
        record_source(diagnostics_.events_by_source, event.source);
        return true;
    } catch (...) {
        const std::scoped_lock lock{diagnostics_mutex_};
        ++diagnostics_.worker_failures;
        return false;
    }
}

std::uint64_t SystemEventCollector::cadence_reset_generation() const noexcept {
    return cadence_reset_generation_.load(std::memory_order_acquire);
}

EventCollectorDiagnostics SystemEventCollector::diagnostics() const noexcept {
    const std::scoped_lock lock{diagnostics_mutex_};
    auto result = diagnostics_;
    result.ring = recorder_.statistics();
    return result;
}

void SystemEventCollector::run(const std::stop_token stop_token) noexcept {
    try {
        auto provider_status = provider_.start(configuration_.provider);
        {
            const std::scoped_lock lock{diagnostics_mutex_};
            diagnostics_.running = true;
            diagnostics_.provider_status = provider_status;
            diagnostics_.capabilities = provider_.capabilities();
            if (provider_status == EventProviderStatus::temporarily_failed) {
                ++diagnostics_.provider_failures;
            }
        }
        auto next_poll = clock_.now();
        while (!stop_token.stop_requested()) {
            {
                std::unique_lock lock{wait_mutex_};
                static_cast<void>(
                    wake_condition_.wait_until(lock, stop_token, next_poll, [] { return false; }));
            }
            if (stop_token.stop_requested()) {
                break;
            }
            const auto started = clock_.now();
            const auto result = provider_.poll(started, *batch_);
            const auto count = std::min(result.event_count, batch_->size());
            EventSourceCounts source_counts{};
            for (std::size_t index = 0U; index < count; ++index) {
                if ((*batch_)[index].observed_at == core::MonotonicTimePoint{}) {
                    (*batch_)[index].observed_at = started;
                }
                recorder_.append((*batch_)[index]);
                record_source(source_counts, (*batch_)[index].source);
                if (resets_sampling_cadence((*batch_)[index])) {
                    cadence_reset_generation_.fetch_add(1U, std::memory_order_release);
                }
                if (configuration_.automatic_system_event_capture &&
                    incident_capture_sink_ != nullptr) {
                    const auto trigger = automatic_trigger((*batch_)[index].kind);
                    if (!trigger) continue;
                    const auto capture = incident_capture_sink_->request_incident_capture(
                        (*batch_)[index].observed_at,
                        core::IncidentCaptureTrigger{core::IncidentTriggerKind::automatic,
                                                     trigger->resource, 1.0, 0.0, 1.0,
                                                     trigger->signal});
                    const std::scoped_lock lock{diagnostics_mutex_};
                    ++diagnostics_.automatic_event_requests;
                    if (capture == core::IncidentCaptureRequestResult::started)
                        ++diagnostics_.automatic_event_captures_started;
                    else if (capture == core::IncidentCaptureRequestResult::merged)
                        ++diagnostics_.automatic_event_captures_merged;
                    else
                        ++diagnostics_.automatic_event_capture_rejections;
                }
            }
            poll_timing_.record(
                std::max(core::MonotonicClock::duration::zero(), clock_.now() - started));
            {
                const std::scoped_lock lock{diagnostics_mutex_};
                const auto prior = diagnostics_.provider_status;
                diagnostics_.provider_status = result.status;
                ++diagnostics_.poll_count;
                diagnostics_.events_recorded += count;
                diagnostics_.events_by_source.power += source_counts.power;
                diagnostics_.events_by_source.device += source_counts.device;
                diagnostics_.events_by_source.audio += source_counts.audio;
                diagnostics_.events_by_source.service_manager += source_counts.service_manager;
                diagnostics_.events_by_source.security += source_counts.security;
                diagnostics_.events_by_source.update += source_counts.update;
                diagnostics_.events_by_source.application += source_counts.application;
                diagnostics_.events_by_source.network += source_counts.network;
                diagnostics_.events_by_source.graphics += source_counts.graphics;
                diagnostics_.events_by_source.storage += source_counts.storage;
                diagnostics_.events_by_source.process += source_counts.process;
                diagnostics_.native_events_dropped = result.native_events_dropped;
                if (result.status == EventProviderStatus::temporarily_failed) {
                    ++diagnostics_.provider_failures;
                } else if (prior == EventProviderStatus::temporarily_failed) {
                    ++diagnostics_.provider_recoveries;
                }
                diagnostics_.poll_timing = poll_timing_.summary();
            }
            next_poll += configuration_.poll_interval;
            if (next_poll <= clock_.now()) {
                next_poll = clock_.now() + configuration_.poll_interval;
            }
        }
        provider_.stop();
    } catch (...) {
        provider_.stop();
        const std::scoped_lock lock{diagnostics_mutex_};
        ++diagnostics_.worker_failures;
        diagnostics_.provider_status = EventProviderStatus::temporarily_failed;
        core::Logger::write(core::LogLevel::error, "events", "System event collector failed");
    }
    const std::scoped_lock lock{diagnostics_mutex_};
    diagnostics_.running = false;
}

} // namespace blackbox::telemetry
