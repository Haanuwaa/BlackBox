#pragma once

#include "telemetry/types.hpp"

#include <cstddef>
#include <memory>

namespace blackbox::telemetry::windows {

class WindowsProcessCollector final {
public:
    WindowsProcessCollector() noexcept;
    ~WindowsProcessCollector();

    WindowsProcessCollector(const WindowsProcessCollector&) = delete;
    WindowsProcessCollector& operator=(const WindowsProcessCollector&) = delete;

    [[nodiscard]] MetricStatus collect(bool collect_counters,
                                       bool resolve_paths,
                                       RawTelemetrySnapshot& destination);
    [[nodiscard]] std::size_t cache_size() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_{};
};

} // namespace blackbox::telemetry::windows
