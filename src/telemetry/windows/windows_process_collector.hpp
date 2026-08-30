#pragma once

#include "telemetry/types.hpp"

#include <cstddef>
#include <memory>

namespace blackbox::telemetry::windows {

// Persistent process handles improve the common path, but an unbounded
// one-handle-per-live-process cache makes the application's handle count track
// unrelated desktop process churn. Keep a small fixed hot set and use
// short-lived RAII handles for every other identity.
inline constexpr std::size_t maximum_cached_process_handles = 16U;

struct WindowsProcessCollectorDiagnostics {
    std::uint64_t handles_opened{};
    std::uint64_t handles_reused{};
    std::uint64_t handle_open_failures{};
    friend constexpr bool operator==(const WindowsProcessCollectorDiagnostics&,
                                     const WindowsProcessCollectorDiagnostics&) = default;
};

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
    [[nodiscard]] std::size_t cached_handle_count() const noexcept;
    [[nodiscard]] WindowsProcessCollectorDiagnostics diagnostics() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_{};
};

} // namespace blackbox::telemetry::windows
