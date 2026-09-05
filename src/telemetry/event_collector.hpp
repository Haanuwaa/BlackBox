#pragma once

#include "core/circular_recorder.hpp"
#include "core/clock.hpp"
#include "core/incident.hpp"
#include "core/system_event.hpp"
#include "telemetry/collection_timing.hpp"
#include "telemetry/event_provider.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace blackbox::telemetry {

inline constexpr std::size_t default_event_ring_capacity = 4'096U;
inline constexpr std::size_t maximum_event_ring_capacity = 65'536U;
inline constexpr std::size_t maximum_events_per_poll = 256U;

struct EventCollectorConfiguration {
    std::chrono::milliseconds poll_interval{250};
    std::size_t ring_capacity{default_event_ring_capacity};
    EventProviderConfiguration provider{};
    bool automatic_system_event_capture{true};
    friend constexpr bool operator==(const EventCollectorConfiguration&,
                                     const EventCollectorConfiguration&) = default;
};

struct EventSourceCounts {
    std::uint64_t power{};
    std::uint64_t device{};
    std::uint64_t audio{};
    std::uint64_t service_manager{};
    std::uint64_t security{};
    std::uint64_t update{};
    std::uint64_t application{};
    std::uint64_t network{};
    std::uint64_t graphics{};
    std::uint64_t storage{};
    std::uint64_t process{};
    friend constexpr bool operator==(const EventSourceCounts&, const EventSourceCounts&) = default;
};

struct EventCollectorDiagnostics {
    bool running{};
    EventCollectorConfiguration configuration{};
    EventProviderCapabilities capabilities{};
    EventProviderStatus provider_status{EventProviderStatus::complete};
    std::uint64_t poll_count{};
    std::uint64_t events_recorded{};
    std::uint64_t external_events_recorded{};
    EventSourceCounts events_by_source{};
    std::uint64_t provider_failures{};
    std::uint64_t provider_recoveries{};
    std::uint64_t native_events_dropped{};
    std::uint64_t worker_failures{};
    std::uint64_t automatic_event_requests{};
    std::uint64_t automatic_event_captures_started{};
    std::uint64_t automatic_event_captures_merged{};
    std::uint64_t automatic_event_capture_rejections{};
    core::RecorderStatistics ring{};
    CollectionTimingSummary poll_timing{};
};

class ISystemEventHistory {
public:
    virtual ~ISystemEventHistory() = default;
    [[nodiscard]] virtual core::RecorderSnapshot<core::SystemEvent>
    snapshot(std::size_t maximum_events) const = 0;
};

class ISystemEventSink {
public:
    virtual ~ISystemEventSink() = default;
    // External normalized events must already satisfy the core privacy
    // contract. Returns false when an event is malformed or cannot be kept.
    [[nodiscard]] virtual bool record_external_event(const core::SystemEvent& event) noexcept = 0;
};

// A power callback can arrive while the ordinary telemetry provider is already
// sampling. This tiny generation boundary lets the collector rebase its next
// deadline without polling the native event provider or copying event history.
struct SamplingCadenceState {
    std::uint64_t generation{};
    std::uint64_t native_resumes{};
    core::MonotonicTimePoint last_resume_at{};
};

class ISamplingCadenceResetSignal {
public:
    virtual ~ISamplingCadenceResetSignal() = default;
    [[nodiscard]] virtual std::uint64_t cadence_reset_generation() const noexcept = 0;
    [[nodiscard]] virtual SamplingCadenceState cadence_state() const noexcept {
        return {cadence_reset_generation()};
    }
};

class SystemEventCollector final : public ISystemEventHistory,
                                   public ISystemEventSink,
                                   public ISamplingCadenceResetSignal {
public:
    SystemEventCollector(ISystemEventProvider& provider, const core::IMonotonicClock& clock,
                         EventCollectorConfiguration configuration = {});
    ~SystemEventCollector();

    SystemEventCollector(const SystemEventCollector&) = delete;
    SystemEventCollector& operator=(const SystemEventCollector&) = delete;

    void start();
    void stop() noexcept;
    void reconfigure(EventCollectorConfiguration configuration);
    // The sink is configured by the composition root before start and must
    // outlive a running collector.
    void set_incident_capture_sink(core::IIncidentCaptureRequestSink* sink) noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] core::RecorderSnapshot<core::SystemEvent>
    snapshot(std::size_t maximum_events) const override;
    [[nodiscard]] bool record_external_event(const core::SystemEvent& event) noexcept override;
    [[nodiscard]] std::uint64_t cadence_reset_generation() const noexcept override;
    [[nodiscard]] SamplingCadenceState cadence_state() const noexcept override;
    [[nodiscard]] EventCollectorDiagnostics diagnostics() const noexcept;

private:
    void run(std::stop_token stop_token) noexcept;
    static void validate(const EventCollectorConfiguration& configuration);

    ISystemEventProvider& provider_;
    const core::IMonotonicClock& clock_;
    EventCollectorConfiguration configuration_{};
    core::CircularRecorder<core::SystemEvent> recorder_;
    std::unique_ptr<std::array<core::SystemEvent, maximum_events_per_poll>> batch_{};
    CollectionTimingWindow poll_timing_{};

    mutable std::mutex diagnostics_mutex_{};
    EventCollectorDiagnostics diagnostics_{};
    mutable std::mutex lifecycle_mutex_{};
    std::mutex wait_mutex_{};
    std::condition_variable_any wake_condition_{};
    std::jthread worker_{};
    core::IIncidentCaptureRequestSink* incident_capture_sink_{};
    std::atomic<std::uint64_t> cadence_reset_generation_{};
    mutable std::mutex cadence_mutex_{};
    SamplingCadenceState cadence_state_{};
};

} // namespace blackbox::telemetry
