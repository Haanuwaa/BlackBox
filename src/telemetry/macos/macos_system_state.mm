#include "telemetry/macos/macos_system_state.hpp"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>

namespace blackbox::telemetry::macos {

struct MacosMemoryPressureMonitor::State {
    std::atomic<MemoryPressureState> value{MemoryPressureState::unknown};
    std::atomic<MetricStatus> status{MetricStatus::temporarily_unavailable};
};

struct MacosSchedulerLatencyMonitor::State {
    std::atomic<std::int64_t> expected_nanoseconds{};
    std::atomic<double> delay_seconds{};
    std::atomic<MetricStatus> status{MetricStatus::temporarily_unavailable};
};

namespace {

[[nodiscard]] MemoryPressureState pressure_state_from_flags(const unsigned long flags) noexcept {
    if ((flags & DISPATCH_MEMORYPRESSURE_CRITICAL) != 0U) {
        return MemoryPressureState::critical;
    }
    if ((flags & DISPATCH_MEMORYPRESSURE_WARN) != 0U) {
        return MemoryPressureState::warning;
    }
    if ((flags & DISPATCH_MEMORYPRESSURE_NORMAL) != 0U) {
        return MemoryPressureState::normal;
    }
    return MemoryPressureState::unknown;
}

[[nodiscard]] std::int64_t steady_nanoseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

MacosMemoryPressureMonitor::MacosMemoryPressureMonitor() noexcept {
    try {
        state_ = std::make_shared<State>();
    } catch (...) {
        return;
    }
    auto source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0U,
        DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
            DISPATCH_MEMORYPRESSURE_CRITICAL,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (source == nullptr) {
        state_->status.store(MetricStatus::inaccessible, std::memory_order_relaxed);
        return;
    }
    source_ = (__bridge_retained void*)source;
    const std::weak_ptr<State> weak_state{state_};
    dispatch_source_set_event_handler(source, ^{
        const auto shared = weak_state.lock();
        if (!shared) return;
        const auto value = pressure_state_from_flags(dispatch_source_get_data(source));
        if (value == MemoryPressureState::unknown) return;
        shared->value.store(value, std::memory_order_relaxed);
        shared->status.store(MetricStatus::available, std::memory_order_release);
    });
    dispatch_activate(source);
}

MacosMemoryPressureMonitor::~MacosMemoryPressureMonitor() {
    if (source_ == nullptr) return;
    auto source = (__bridge_transfer dispatch_source_t)source_;
    source_ = nullptr;
    dispatch_source_set_event_handler(source, nil);
    dispatch_source_cancel(source);
}

MetricValue<MemoryPressureState> MacosMemoryPressureMonitor::state() const noexcept {
    if (!state_) {
        return MetricValue<MemoryPressureState>::unavailable(
            MetricStatus::temporarily_unavailable);
    }
    const auto status = state_->status.load(std::memory_order_acquire);
    if (status != MetricStatus::available) {
        return MetricValue<MemoryPressureState>::unavailable(status);
    }
    return MetricValue<MemoryPressureState>::available(
        state_->value.load(std::memory_order_relaxed));
}

MacosSchedulerLatencyMonitor::MacosSchedulerLatencyMonitor() noexcept {
    constexpr std::int64_t interval_nanoseconds = 1'000'000'000LL;
    constexpr std::int64_t allowed_leeway_nanoseconds = 10'000'000LL;
    constexpr std::int64_t suspend_gap_nanoseconds = 10'000'000'000LL;
    try {
        state_ = std::make_shared<State>();
    } catch (...) {
        return;
    }
    auto source = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0U, 0U,
                                         dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (source == nullptr) {
        state_->status.store(MetricStatus::inaccessible, std::memory_order_relaxed);
        return;
    }
    source_ = (__bridge_retained void*)source;
    state_->expected_nanoseconds.store(steady_nanoseconds() + interval_nanoseconds,
                                       std::memory_order_relaxed);
    dispatch_source_set_timer(source, dispatch_time(DISPATCH_TIME_NOW, interval_nanoseconds),
                              interval_nanoseconds, allowed_leeway_nanoseconds);
    const std::weak_ptr<State> weak_state{state_};
    dispatch_source_set_event_handler(source, ^{
        const auto shared = weak_state.lock();
        if (!shared) return;
        const auto now = steady_nanoseconds();
        const auto expected = shared->expected_nanoseconds.load(std::memory_order_relaxed);
        const auto lateness = now - expected;
        shared->expected_nanoseconds.store(now + interval_nanoseconds,
                                           std::memory_order_relaxed);
        if (lateness >= suspend_gap_nanoseconds) {
            shared->status.store(MetricStatus::temporarily_unavailable,
                                 std::memory_order_release);
            return;
        }
        const auto excess = std::max<std::int64_t>(0, lateness - allowed_leeway_nanoseconds);
        shared->delay_seconds.store(static_cast<double>(excess) / 1'000'000'000.0,
                                    std::memory_order_relaxed);
        shared->status.store(MetricStatus::available, std::memory_order_release);
    });
    dispatch_activate(source);
}

MacosSchedulerLatencyMonitor::~MacosSchedulerLatencyMonitor() {
    if (source_ == nullptr) return;
    auto source = (__bridge_transfer dispatch_source_t)source_;
    source_ = nullptr;
    dispatch_source_set_event_handler(source, nil);
    dispatch_source_cancel(source);
}

MetricValue<Seconds> MacosSchedulerLatencyMonitor::state() const noexcept {
    if (!state_) {
        return MetricValue<Seconds>::unavailable(MetricStatus::temporarily_unavailable);
    }
    const auto status = state_->status.load(std::memory_order_acquire);
    if (status != MetricStatus::available) return MetricValue<Seconds>::unavailable(status);
    return MetricValue<Seconds>::available(
        Seconds{state_->delay_seconds.load(std::memory_order_relaxed)});
}

MetricValue<bool> macos_low_power_mode() noexcept {
    @autoreleasepool {
        if (@available(macOS 12.0, *)) {
            NSProcessInfo* process_info = NSProcessInfo.processInfo;
            if (process_info == nil) {
                return MetricValue<bool>::unavailable(MetricStatus::temporarily_unavailable);
            }
            return MetricValue<bool>::available(process_info.lowPowerModeEnabled != NO);
        }
    }
    return MetricValue<bool>::unavailable(MetricStatus::unsupported);
}

MetricValue<ThermalPressureState> macos_thermal_pressure_state() noexcept {
    @autoreleasepool {
        NSProcessInfo* process_info = NSProcessInfo.processInfo;
        if (process_info == nil) {
            return MetricValue<ThermalPressureState>::unavailable(
                MetricStatus::temporarily_unavailable);
        }
        switch (process_info.thermalState) {
        case NSProcessInfoThermalStateNominal:
            return MetricValue<ThermalPressureState>::available(ThermalPressureState::nominal);
        case NSProcessInfoThermalStateFair:
            return MetricValue<ThermalPressureState>::available(ThermalPressureState::fair);
        case NSProcessInfoThermalStateSerious:
            return MetricValue<ThermalPressureState>::available(ThermalPressureState::serious);
        case NSProcessInfoThermalStateCritical:
            return MetricValue<ThermalPressureState>::available(ThermalPressureState::critical);
        }
    }
    return MetricValue<ThermalPressureState>::available(ThermalPressureState::unknown);
}

MetricValue<ProcessId> macos_frontmost_process_id() noexcept {
    @autoreleasepool {
        NSRunningApplication* application = NSWorkspace.sharedWorkspace.frontmostApplication;
        if (application == nil || application.processIdentifier <= 0) {
            return MetricValue<ProcessId>::unavailable(MetricStatus::temporarily_unavailable);
        }
        const auto pid = static_cast<std::uint64_t>(application.processIdentifier);
        if (pid > std::numeric_limits<std::uint32_t>::max()) {
            return MetricValue<ProcessId>::unavailable(MetricStatus::temporarily_unavailable);
        }
        return MetricValue<ProcessId>::available(ProcessId{static_cast<std::uint32_t>(pid)});
    }
}

GpuInventoryEvidence macos_gpu_inventory() noexcept {
    @autoreleasepool {
        NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
        if (devices == nil ||
            devices.count > static_cast<NSUInteger>(std::numeric_limits<std::uint32_t>::max())) {
            return {};
        }
        std::uint32_t integrated{};
        std::uint32_t discrete{};
        for (id<MTLDevice> device in devices) {
            if (device == nil) continue;
            if (device.lowPower != NO)
                ++integrated;
            else
                ++discrete;
        }
        const auto count = integrated + discrete;
        GpuInventoryEvidence result{};
        result.device_count = MetricValue<std::uint32_t>::available(count);
        result.integrated_device_count = MetricValue<std::uint32_t>::available(integrated);
        result.discrete_device_count = MetricValue<std::uint32_t>::available(discrete);
        result.unknown_device_count = MetricValue<std::uint32_t>::available(0U);
        result.render_device_available =
            MetricValue<bool>::available(MTLCreateSystemDefaultDevice() != nil);
        return result;
    }
}

} // namespace blackbox::telemetry::macos
