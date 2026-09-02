#include "platform/macos/macos_app_performance_monitor.hpp"

#import <Foundation/Foundation.h>
#import <MetricKit/MetricKit.h>

#include <atomic>
#include <cmath>
#include <memory>

namespace blackbox::platform::macos::detail {

struct MetricState final {
    std::atomic<AppPerformanceReportStatus> status{AppPerformanceReportStatus::unsupported};
    std::atomic<std::uint64_t> metric_payloads{};
    std::atomic<std::uint64_t> diagnostic_payloads{};
    std::atomic<double> cumulative_cpu_seconds{};
    std::atomic<double> cumulative_gpu_seconds{};
    std::atomic<std::uint64_t> hang_diagnostics{};
    std::atomic<double> hang_duration_seconds{};
};

[[nodiscard]] double seconds(NSMeasurement<NSUnitDuration*>* measurement) noexcept {
    if (measurement == nil) return 0.0;
    @try {
        const auto value =
            [[measurement measurementByConvertingToUnit:NSUnitDuration.seconds] doubleValue];
        return std::isfinite(value) && value >= 0.0 ? value : 0.0;
    } @catch (__unused NSException* exception) {
        return 0.0;
    }
}

} // namespace blackbox::platform::macos::detail

@interface BBMetricKitSubscriber : NSObject <MXMetricManagerSubscriber> {
    std::weak_ptr<blackbox::platform::macos::detail::MetricState> _state;
}
- (instancetype)initWithState:
    (std::shared_ptr<blackbox::platform::macos::detail::MetricState>)state;
@end

@implementation BBMetricKitSubscriber

- (instancetype)initWithState:
    (std::shared_ptr<blackbox::platform::macos::detail::MetricState>)state {
    self = [super init];
    if (self != nil) _state = state;
    return self;
}

- (void)didReceiveMetricPayloads:(NSArray<MXMetricPayload*>*)payloads {
    const auto state = _state.lock();
    if (!state) return;
    double cpu_seconds{};
    double gpu_seconds{};
    for (MXMetricPayload* payload in payloads) {
        if (payload == nil) continue;
        cpu_seconds += blackbox::platform::macos::detail::seconds(
            payload.cpuMetrics.cumulativeCPUTime);
        gpu_seconds += blackbox::platform::macos::detail::seconds(
            payload.gpuMetrics.cumulativeGPUTime);
    }
    state->metric_payloads.fetch_add(static_cast<std::uint64_t>(payloads.count),
                                     std::memory_order_relaxed);
    state->cumulative_cpu_seconds.fetch_add(cpu_seconds, std::memory_order_relaxed);
    state->cumulative_gpu_seconds.fetch_add(gpu_seconds, std::memory_order_relaxed);
    state->status.store(blackbox::platform::macos::AppPerformanceReportStatus::available,
                        std::memory_order_release);
}

- (void)didReceiveDiagnosticPayloads:(NSArray<MXDiagnosticPayload*>*)payloads {
    const auto state = _state.lock();
    if (!state) return;
    std::uint64_t hangs{};
    double hang_seconds{};
    for (MXDiagnosticPayload* payload in payloads) {
        if (payload == nil) continue;
        for (MXHangDiagnostic* diagnostic in payload.hangDiagnostics) {
            ++hangs;
            hang_seconds += blackbox::platform::macos::detail::seconds(diagnostic.hangDuration);
        }
    }
    state->diagnostic_payloads.fetch_add(static_cast<std::uint64_t>(payloads.count),
                                         std::memory_order_relaxed);
    state->hang_diagnostics.fetch_add(hangs, std::memory_order_relaxed);
    state->hang_duration_seconds.fetch_add(hang_seconds, std::memory_order_relaxed);
    state->status.store(blackbox::platform::macos::AppPerformanceReportStatus::available,
                        std::memory_order_release);
}

@end

namespace blackbox::platform::macos {

struct MacosAppPerformanceMonitor::Implementation final {
    std::shared_ptr<detail::MetricState> state{};
    BBMetricKitSubscriber* subscriber{};
    bool started{};
};

MacosAppPerformanceMonitor::MacosAppPerformanceMonitor() noexcept {
    try {
        implementation_ = std::make_unique<Implementation>();
        implementation_->state = std::make_shared<detail::MetricState>();
    } catch (...) {
        implementation_.reset();
    }
}

MacosAppPerformanceMonitor::~MacosAppPerformanceMonitor() { stop(); }

void MacosAppPerformanceMonitor::start() noexcept {
    if (!implementation_ || !implementation_->state || implementation_->started) return;
    @autoreleasepool {
        if (@available(macOS 12.0, *)) {
            MXMetricManager* manager = MXMetricManager.sharedManager;
            if (manager == nil) {
                implementation_->state->status.store(AppPerformanceReportStatus::failed,
                                                      std::memory_order_release);
                return;
            }
            implementation_->subscriber =
                [[BBMetricKitSubscriber alloc] initWithState:implementation_->state];
            if (implementation_->subscriber == nil) {
                implementation_->state->status.store(AppPerformanceReportStatus::failed,
                                                      std::memory_order_release);
                return;
            }
            [manager addSubscriber:implementation_->subscriber];
            implementation_->started = true;
            implementation_->state->status.store(AppPerformanceReportStatus::awaiting_report,
                                                  std::memory_order_release);
        } else {
            implementation_->state->status.store(AppPerformanceReportStatus::unsupported,
                                                  std::memory_order_release);
        }
    }
}

void MacosAppPerformanceMonitor::stop() noexcept {
    if (!implementation_ || !implementation_->started) return;
    @autoreleasepool {
        if (@available(macOS 12.0, *)) {
            [MXMetricManager.sharedManager removeSubscriber:implementation_->subscriber];
        }
        implementation_->subscriber = nil;
        implementation_->started = false;
    }
}

AppPerformanceSnapshot MacosAppPerformanceMonitor::snapshot() const noexcept {
    if (!implementation_ || !implementation_->state) {
        return AppPerformanceSnapshot{AppPerformanceReportStatus::failed};
    }
    const auto& state = *implementation_->state;
    return AppPerformanceSnapshot{
        state.status.load(std::memory_order_acquire),
        state.metric_payloads.load(std::memory_order_relaxed),
        state.diagnostic_payloads.load(std::memory_order_relaxed),
        state.cumulative_cpu_seconds.load(std::memory_order_relaxed),
        state.cumulative_gpu_seconds.load(std::memory_order_relaxed),
        state.hang_diagnostics.load(std::memory_order_relaxed),
        state.hang_duration_seconds.load(std::memory_order_relaxed)};
}

} // namespace blackbox::platform::macos
