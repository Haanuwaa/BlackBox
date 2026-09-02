#pragma once

#include <cstdint>
#include <memory>

namespace blackbox::platform::macos {

enum class AppPerformanceReportStatus : std::uint8_t {
    unsupported,
    awaiting_report,
    available,
    failed,
};

struct AppPerformanceSnapshot {
    AppPerformanceReportStatus status{AppPerformanceReportStatus::unsupported};
    std::uint64_t metric_payloads{};
    std::uint64_t diagnostic_payloads{};
    double cumulative_cpu_seconds{};
    double cumulative_gpu_seconds{};
    std::uint64_t hang_diagnostics{};
    double hang_duration_seconds{};
};

// Ingests delayed, app-scoped MetricKit payloads. MetricKit delivery is
// asynchronous and commonly daily; this is never presented as live or
// whole-system telemetry.
class MacosAppPerformanceMonitor final {
public:
    MacosAppPerformanceMonitor() noexcept;
    ~MacosAppPerformanceMonitor();

    MacosAppPerformanceMonitor(const MacosAppPerformanceMonitor&) = delete;
    MacosAppPerformanceMonitor& operator=(const MacosAppPerformanceMonitor&) = delete;

    void start() noexcept;
    void stop() noexcept;
    [[nodiscard]] AppPerformanceSnapshot snapshot() const noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_{};
};

} // namespace blackbox::platform::macos
