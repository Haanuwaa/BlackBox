#pragma once

namespace blackbox::app {
struct ExitReadiness {
    bool settings_draft{};
    bool incident_draft{};
    bool capture_pending{};
    bool writes_pending{};
    bool recovery_pending{};
    bool maintenance_pending{};
    bool export_pending{};
    [[nodiscard]] bool ready() const noexcept {
        return !settings_draft && !incident_draft && !capture_pending &&
               !writes_pending && !recovery_pending && !maintenance_pending && !export_pending;
    }
};
} // namespace blackbox::app
