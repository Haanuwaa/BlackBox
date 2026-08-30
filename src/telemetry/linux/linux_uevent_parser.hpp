#pragma once

#include "telemetry/event_provider.hpp"

#include <optional>
#include <string_view>

namespace blackbox::telemetry::linux {

// Converts one bounded kernel uevent to identifier-free portable evidence.
// Known storage, graphics, network, and audio classes use their portable
// source and kind; all other accepted classes remain generic device churn.
// DEVPATH, DEVNAME, PRODUCT, INTERFACE, MODALIAS, and all other payload fields
// are intentionally ignored.
[[nodiscard]] std::optional<core::SystemEvent> normalized_linux_uevent(
    std::string_view payload,
    const EventProviderConfiguration& configuration) noexcept;

} // namespace blackbox::telemetry::linux
