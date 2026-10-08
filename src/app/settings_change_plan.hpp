#pragma once

#include "app/product_settings.hpp"
#include "telemetry/collector.hpp"

namespace blackbox::app {

struct SettingsChangePlan {
    bool recorder{};
    bool detector{};
    bool events{};
    bool foreground{};
    bool lifecycle{};
    [[nodiscard]] bool interrupts_capture() const noexcept {
        return recorder || detector || events || foreground || lifecycle;
    }
};

[[nodiscard]] inline SettingsChangePlan settings_change_plan(
    const ProductSettings& before, const ProductSettings& after,
    const telemetry::RecorderConfiguration& recorder_before,
    const telemetry::RecorderConfiguration& recorder_after) noexcept {
    return {
        recorder_before != recorder_after,
        detector_configuration(before) != detector_configuration(after) ||
            before.automatic_detection_enabled != after.automatic_detection_enabled,
        event_provider_configuration(before) != event_provider_configuration(after) ||
            before.automatic_detection_enabled != after.automatic_detection_enabled,
        before.record_foreground_application != after.record_foreground_application,
        before.record_process_lifecycle != after.record_process_lifecycle};
}

} // namespace blackbox::app
