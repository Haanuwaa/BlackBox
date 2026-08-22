#pragma once

#include "core/clock.hpp"

#include <compare>
#include <cstdint>

namespace blackbox::core {

// Privacy-bounded event evidence. Native providers intentionally normalize
// events to these enums and never retain Event Log messages, device IDs,
// endpoint IDs, window titles, or other free-form payloads.
enum class SystemEventSource : std::uint8_t {
    power,
    device,
    audio,
    service_control_manager,
    defender,
    windows_update,
    application,
    network,
    graphics,
    storage,
    process,
};

enum class SystemEventKind : std::uint8_t {
    suspend,
    resume_automatic,
    resume_user,
    device_enumerated,
    device_started,
    device_removed,
    audio_endpoint_added,
    audio_endpoint_removed,
    audio_endpoint_state_changed,
    audio_default_changed,
    service_state_changed,
    service_unexpected_stop,
    defender_scan_started,
    defender_scan_completed,
    defender_threat_detected,
    defender_action,
    defender_configuration_changed,
    update_activity_started,
    update_succeeded,
    update_failed,
    application_crash,
    application_hang,
    dns_resolution_timeout,
    display_driver_recovery,
    storage_io_retry,
    process_started,
    process_exited,
};

enum class SystemEventLevel : std::uint8_t {
    informational,
    warning,
    error,
};

struct SystemEvent {
    MonotonicTimePoint observed_at{};
    std::int64_t source_utc_milliseconds{};
    bool has_source_utc_time{};
    SystemEventSource source{SystemEventSource::device};
    SystemEventKind kind{SystemEventKind::device_enumerated};
    SystemEventLevel level{SystemEventLevel::informational};
    std::uint32_t native_event_id{};
    std::uint32_t detail{};
    // Present only for opt-in process lifecycle evidence. Event-log and
    // device providers never populate process identity fields.
    bool has_process_identity{};
    std::uint32_t process_pid{};
    std::uint64_t process_creation_token{};
    friend constexpr bool operator==(const SystemEvent&, const SystemEvent&) = default;
};

} // namespace blackbox::core
