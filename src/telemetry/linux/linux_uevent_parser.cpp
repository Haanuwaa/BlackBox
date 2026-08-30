#include "telemetry/linux/linux_uevent_parser.hpp"

#include <cstdint>

namespace blackbox::telemetry::linux {
namespace {

[[nodiscard]] constexpr std::uint32_t subsystem_detail(
    const std::string_view subsystem) noexcept {
    if (subsystem == "block") return 1U;
    if (subsystem == "drm") return 2U;
    if (subsystem == "input") return 3U;
    if (subsystem == "net") return 4U;
    if (subsystem == "sound") return 5U;
    if (subsystem == "usb") return 6U;
    return 0U;
}

} // namespace

std::optional<core::SystemEvent> normalized_linux_uevent(
    std::string_view payload,
    const EventProviderConfiguration& configuration) noexcept {
    if (payload.empty() || payload.size() > 4096U) {
        return std::nullopt;
    }

    std::string_view action{};
    std::string_view subsystem{};
    bool have_action{};
    bool have_subsystem{};
    while (!payload.empty()) {
        const auto delimiter = payload.find('\0');
        const auto field = payload.substr(0U, delimiter);
        payload = delimiter == std::string_view::npos
                      ? std::string_view{}
                      : payload.substr(delimiter + 1U);
        if (field.starts_with("ACTION=")) {
            if (have_action) return std::nullopt;
            action = field.substr(7U);
            have_action = true;
        } else if (field.starts_with("SUBSYSTEM=")) {
            if (have_subsystem) return std::nullopt;
            subsystem = field.substr(10U);
            have_subsystem = true;
        }
    }
    if (!have_action || !have_subsystem || action.empty() || subsystem.empty()) {
        return std::nullopt;
    }

    core::SystemEvent event{};
    event.level = core::SystemEventLevel::informational;
    event.detail = subsystem_detail(subsystem);
    if (subsystem == "block") {
        if (!configuration.storage_events) return std::nullopt;
        event.source = core::SystemEventSource::storage;
        if (action == "add") {
            event.kind = core::SystemEventKind::storage_device_added;
        } else if (action == "remove") {
            event.kind = core::SystemEventKind::storage_device_removed;
        } else {
            return std::nullopt;
        }
    } else if (subsystem == "drm") {
        if (!configuration.graphics_events || action != "change") return std::nullopt;
        event.source = core::SystemEventSource::graphics;
        event.kind = core::SystemEventKind::display_configuration_changed;
    } else if (subsystem == "net") {
        if (!configuration.network_events ||
            (action != "add" && action != "remove" && action != "change")) {
            return std::nullopt;
        }
        event.source = core::SystemEventSource::network;
        event.kind = core::SystemEventKind::network_connectivity_changed;
    } else if (subsystem == "sound") {
        if (!configuration.audio_device_events) return std::nullopt;
        event.source = core::SystemEventSource::audio;
        if (action == "add") {
            event.kind = core::SystemEventKind::audio_endpoint_added;
        } else if (action == "remove") {
            event.kind = core::SystemEventKind::audio_endpoint_removed;
        } else if (action == "change") {
            event.kind = core::SystemEventKind::audio_endpoint_state_changed;
        } else {
            return std::nullopt;
        }
    } else {
        if (!configuration.device_events) return std::nullopt;
        event.source = core::SystemEventSource::device;
        if (action == "add") {
            event.kind = core::SystemEventKind::device_enumerated;
        } else if (action == "remove") {
            event.kind = core::SystemEventKind::device_removed;
        } else {
            return std::nullopt;
        }
    }
    return event;
}

} // namespace blackbox::telemetry::linux
