#pragma once

#include "telemetry/types.hpp"

#include <cstddef>
#include <memory>

namespace blackbox::telemetry::macos {

class MacosProcessCollector final {
public:
    MacosProcessCollector() noexcept;
    ~MacosProcessCollector();

    MacosProcessCollector(const MacosProcessCollector&) = delete;
    MacosProcessCollector& operator=(const MacosProcessCollector&) = delete;

    [[nodiscard]] MetricStatus collect(bool collect_counters,
                                       bool resolve_paths,
                                       RawTelemetrySnapshot& destination);
    [[nodiscard]] std::size_t cache_size() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_{};
};

} // namespace blackbox::telemetry::macos
