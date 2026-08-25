#pragma once

#include "telemetry/event_provider.hpp"

#include <optional>
#include <string_view>

namespace blackbox::telemetry::linux {

// Converts one bounded kernel uevent to identifier-free portable evidence.
// DEVPATH, DEVNAME, PRODUCT, INTERFACE, MODALIAS, and all other payload fields
// are intentionally ignored.
[[nodiscard]] std::optional<core::SystemEvent> normalized_linux_uevent(
    std::string_view payload,
    const EventProviderConfiguration& configuration) noexcept;

} // namespace blackbox::telemetry::linux
