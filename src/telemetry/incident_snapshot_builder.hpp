#pragma once

#include "core/circular_recorder.hpp"
#include "core/incident.hpp"
#include "telemetry/types.hpp"

#include <memory>
#include <span>

namespace blackbox::telemetry {

[[nodiscard]] std::shared_ptr<const core::IncidentSnapshot> build_incident_snapshot(
    const core::IncidentCaptureWindow& window,
    core::MonotonicTimePoint completion_observed_at,
    const core::RecorderSnapshot<SystemSample>& system_history,
    const core::RecorderSnapshot<ProcessFrame>& process_history,
    std::span<const ProcessInfo> metadata,
    const core::RecorderSnapshot<core::SystemEvent>* event_history = nullptr);

} // namespace blackbox::telemetry
