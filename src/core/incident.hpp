#pragma once

#include "core/clock.hpp"
#include "core/system_event.hpp"

#include <chrono>
#include <compare>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace blackbox::core {

enum class RecordedValueStatus : std::uint8_t {
    available,
    unsupported,
    inaccessible,
    temporarily_unavailable,
};

template <typename T> struct RecordedValue {
    T value{};
    RecordedValueStatus status{RecordedValueStatus::unsupported};

    friend bool operator==(const RecordedValue&, const RecordedValue&) = default;
};

struct IncidentProcessIdentity {
    std::uint32_t pid{};
    std::uint64_t creation_token{};
    friend constexpr auto operator<=>(const IncidentProcessIdentity&,
                                      const IncidentProcessIdentity&) = default;
};

struct IncidentSystemSample {
    MonotonicTimePoint observed_at{};
    RecordedValue<double> cpu_fraction{};
    RecordedValue<std::uint64_t> memory_used_bytes{};
    RecordedValue<std::uint64_t> memory_total_bytes{};
    RecordedValue<double> memory_fraction{};
    RecordedValue<double> disk_read_bytes_per_second{};
    RecordedValue<double> disk_write_bytes_per_second{};
    RecordedValue<double> network_receive_bytes_per_second{};
    RecordedValue<double> network_transmit_bytes_per_second{};
    RecordedValue<double> disk_read_latency_seconds{};
    RecordedValue<double> disk_write_latency_seconds{};
    RecordedValue<double> disk_service_time_seconds{};
    RecordedValue<double> disk_queue_depth{};
    RecordedValue<std::uint64_t> disk_worst_device_id{};
    RecordedValue<std::uint8_t> network_connectivity_level{};
    RecordedValue<std::uint64_t> network_active_interfaces{};
    RecordedValue<std::uint64_t> network_interface_changes{};
    RecordedValue<double> network_tcp_retransmit_fraction{};
    RecordedValue<std::uint64_t> network_tcp_failed_connections{};
    RecordedValue<std::uint64_t> network_tcp_resets{};
    RecordedValue<double> gpu_fraction{};
    RecordedValue<std::uint64_t> gpu_dedicated_memory_bytes{};
    RecordedValue<std::uint64_t> gpu_shared_memory_bytes{};
    RecordedValue<IncidentProcessIdentity> foreground_process{};
    RecordedValue<double> foreground_gpu_fraction{};
    RecordedValue<double> dpc_fraction{};
    RecordedValue<double> interrupt_fraction{};
    RecordedValue<double> dpc_rate{};
    RecordedValue<double> cpu_current_mhz{};
    RecordedValue<double> cpu_max_mhz{};
    RecordedValue<double> cpu_thermal_limit_mhz{};
    RecordedValue<double> cpu_thermal_limit_fraction{};
    RecordedValue<std::uint8_t> power_source{};
    RecordedValue<double> battery_fraction{};
    RecordedValue<bool> battery_saver{};
    RecordedValue<double> system_uptime_seconds{};
    RecordedValue<double> cpu_some_pressure_fraction{};
    RecordedValue<double> memory_some_pressure_fraction{};
    RecordedValue<double> memory_full_pressure_fraction{};
    RecordedValue<double> io_some_pressure_fraction{};
    RecordedValue<double> io_full_pressure_fraction{};
    RecordedValue<std::uint8_t> thermal_pressure_state{};
    friend bool operator==(const IncidentSystemSample&, const IncidentSystemSample&) = default;
};

struct IncidentProcessSample {
    MonotonicTimePoint observed_at{};
    IncidentProcessIdentity identity{};
    RecordedValue<double> cpu_fraction{};
    RecordedValue<std::uint64_t> working_set_bytes{};
    RecordedValue<double> disk_read_bytes_per_second{};
    RecordedValue<double> disk_write_bytes_per_second{};
    friend bool operator==(const IncidentProcessSample&, const IncidentProcessSample&) = default;
};

struct IncidentProcessInfo {
    IncidentProcessIdentity identity{};
    RecordedValue<std::uint32_t> parent_pid{};
    RecordedValue<std::string> name{};
    RecordedValue<std::string> executable_path{};
    friend bool operator==(const IncidentProcessInfo&, const IncidentProcessInfo&) = default;
};

enum class IncidentTriggerKind : std::uint8_t {
    manual,
    automatic,
};

enum class AutomaticIncidentResource : std::uint8_t {
    none,
    cpu,
    memory,
    disk,
    network,
};

enum class AutomaticIncidentSignal : std::uint8_t {
    throughput_or_utilization,
    disk_latency,
    disk_queue_depth,
    network_connectivity,
    network_interface_transition,
    tcp_retransmission,
    tcp_connection_failure,
    tcp_connection_reset,
    application_crash,
    application_hang,
    display_driver_recovery,
    storage_io_retry,
};

struct IncidentCaptureTrigger {
    IncidentTriggerKind kind{IncidentTriggerKind::manual};
    AutomaticIncidentResource resource{AutomaticIncidentResource::none};
    double observed_value{};
    double baseline_value{};
    double score{};
    AutomaticIncidentSignal signal{AutomaticIncidentSignal::throughput_or_utilization};
    friend constexpr bool operator==(const IncidentCaptureTrigger&,
                                     const IncidentCaptureTrigger&) = default;
};

struct IncidentCaptureWindow {
    std::uint64_t sequence{};
    MonotonicTimePoint event_time{};
    MonotonicTimePoint requested_start{};
    MonotonicTimePoint requested_end{};
    std::uint32_t trigger_count{1U};
    std::uint32_t manual_trigger_count{1U};
    std::uint32_t automatic_trigger_count{};
    AutomaticIncidentResource automatic_resource{AutomaticIncidentResource::none};
    double automatic_observed_value{};
    double automatic_baseline_value{};
    double automatic_score{};
    AutomaticIncidentSignal automatic_signal{AutomaticIncidentSignal::throughput_or_utilization};
    friend constexpr bool operator==(const IncidentCaptureWindow&,
                                     const IncidentCaptureWindow&) = default;
};

struct IncidentHeader {
    IncidentCaptureWindow window{};
    MonotonicTimePoint actual_start{};
    MonotonicTimePoint actual_end{};
    std::uint64_t system_recorder_epoch{};
    std::uint64_t process_recorder_epoch{};
    std::uint64_t event_recorder_epoch{};
    friend constexpr bool operator==(const IncidentHeader&, const IncidentHeader&) = default;
};

// Completed incidents are shared only as pointer-to-const work items. The
// vectors have no mutable accessors, so future storage can consume an incident
// without copying or changing the recorder-owned result.
class IncidentSnapshot final {
public:
    IncidentSnapshot(IncidentHeader header, std::vector<IncidentSystemSample> system_samples,
                     std::vector<IncidentProcessInfo> process_metadata,
                     std::vector<IncidentProcessSample> process_samples,
                     std::vector<SystemEvent> system_events = {}) noexcept;

    [[nodiscard]] const IncidentHeader& header() const noexcept;
    [[nodiscard]] std::span<const IncidentSystemSample> system_samples() const noexcept;
    [[nodiscard]] std::span<const IncidentProcessInfo> process_metadata() const noexcept;
    [[nodiscard]] std::span<const IncidentProcessSample> process_samples() const noexcept;
    [[nodiscard]] std::span<const SystemEvent> system_events() const noexcept;

private:
    IncidentHeader header_{};
    std::vector<IncidentSystemSample> system_samples_{};
    std::vector<IncidentProcessInfo> process_metadata_{};
    std::vector<IncidentProcessSample> process_samples_{};
    std::vector<SystemEvent> system_events_{};
};

class IIncidentWorkSource {
public:
    virtual ~IIncidentWorkSource() = default;

    [[nodiscard]] virtual std::shared_ptr<const IncidentSnapshot>
    wait_pop(std::stop_token stop_token) noexcept = 0;
    [[nodiscard]] virtual std::shared_ptr<const IncidentSnapshot> try_pop() noexcept = 0;
};

enum class IncidentCaptureRequestResult : std::uint8_t {
    started,
    merged,
    queue_full,
    stopped,
};

class IIncidentCaptureRequestSink {
public:
    virtual ~IIncidentCaptureRequestSink() = default;
    [[nodiscard]] virtual IncidentCaptureRequestResult
    request_incident_capture(MonotonicTimePoint event_time,
                             IncidentCaptureTrigger trigger) noexcept = 0;
};

enum class IncidentCapturePhase : std::uint8_t {
    idle,
    collecting_post_window,
    constructing_snapshot,
    queued,
    queue_full,
    stopped,
};

struct IncidentCaptureStatus {
    IncidentCapturePhase phase{IncidentCapturePhase::stopped};
    bool accepting{};
    bool can_request{};
    bool has_pending_window{};
    IncidentCaptureWindow pending_window{};
    std::size_t queue_size{};
    std::size_t queue_capacity{};
    std::uint64_t captures_started{};
    std::uint64_t capture_requests_merged{};
    std::uint64_t incidents_completed{};
    std::uint64_t queue_rejections{};
    std::uint64_t snapshot_failures{};
    std::uint64_t captures_cancelled{};
    friend constexpr bool operator==(const IncidentCaptureStatus&,
                                     const IncidentCaptureStatus&) = default;
};

// One pending post-window is allowed. Requests that arrive while it is active
// merge into that window and extend its end. Completed immutable incidents use
// a fixed-capacity FIFO; queue capacity also reserves any in-progress snapshot.
class IncidentCaptureCoordinator final : public IIncidentWorkSource {
public:
    explicit IncidentCaptureCoordinator(std::size_t queue_capacity);

    IncidentCaptureCoordinator(const IncidentCaptureCoordinator&) = delete;
    IncidentCaptureCoordinator& operator=(const IncidentCaptureCoordinator&) = delete;

    void start_accepting() noexcept;
    void stop_accepting() noexcept;

    [[nodiscard]] IncidentCaptureRequestResult
    request(MonotonicTimePoint event_time, std::chrono::nanoseconds pre_window,
            std::chrono::nanoseconds post_window, IncidentCaptureTrigger trigger = {}) noexcept;
    [[nodiscard]] std::optional<IncidentCaptureWindow>
    try_begin_snapshot(MonotonicTimePoint observed_at) noexcept;
    void finish_snapshot(std::shared_ptr<const IncidentSnapshot> snapshot) noexcept;
    void cancel_pending() noexcept;

    [[nodiscard]] std::shared_ptr<const IncidentSnapshot>
    wait_pop(std::stop_token stop_token) noexcept override;
    [[nodiscard]] std::shared_ptr<const IncidentSnapshot> try_pop() noexcept override;
    [[nodiscard]] IncidentCaptureStatus status() const noexcept;

private:
    [[nodiscard]] std::size_t reserved_slots_locked() const noexcept;

    const std::size_t queue_capacity_{};
    mutable std::mutex mutex_{};
    std::condition_variable_any work_available_{};
    std::deque<std::shared_ptr<const IncidentSnapshot>> queue_{};
    std::optional<IncidentCaptureWindow> pending_{};
    bool snapshot_in_progress_{};
    bool accepting_{};
    bool last_request_rejected_{};
    std::uint64_t next_sequence_{1U};
    std::uint64_t captures_started_{};
    std::uint64_t capture_requests_merged_{};
    std::uint64_t incidents_completed_{};
    std::uint64_t queue_rejections_{};
    std::uint64_t snapshot_failures_{};
    std::uint64_t captures_cancelled_{};
};

} // namespace blackbox::core
