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
    if (!configuration.device_events || payload.empty() || payload.size() > 4096U) {
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

    core::SystemEventKind kind{};
    if (action == "add") {
        kind = core::SystemEventKind::device_enumerated;
    } else if (action == "remove") {
        kind = core::SystemEventKind::device_removed;
    } else {
        return std::nullopt;
    }

    core::SystemEvent event{};
    event.source = core::SystemEventSource::device;
    event.kind = kind;
    event.level = core::SystemEventLevel::informational;
    event.detail = subsystem_detail(subsystem);
    return event;
}

} // namespace blackbox::telemetry::linux
