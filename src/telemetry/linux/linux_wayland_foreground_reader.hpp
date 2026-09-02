#pragma once

#include "telemetry/types.hpp"

#include <memory>

namespace blackbox::telemetry::linux {

// Optional compositor-specific foreground application evidence. This reader
// never exposes an app_id, title, native handle, or synthetic PID.
class LinuxWaylandForegroundReader final {
public:
    LinuxWaylandForegroundReader() noexcept;
    ~LinuxWaylandForegroundReader();

    LinuxWaylandForegroundReader(const LinuxWaylandForegroundReader&) = delete;
    LinuxWaylandForegroundReader& operator=(const LinuxWaylandForegroundReader&) = delete;

    [[nodiscard]] MetricStatus status() const noexcept;
    [[nodiscard]] bool candidate() const noexcept;
    [[nodiscard]] MetricValue<OpaqueApplicationIdentity> read() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_{};
};

} // namespace blackbox::telemetry::linux
