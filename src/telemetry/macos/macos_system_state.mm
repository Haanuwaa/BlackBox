#include "telemetry/macos/macos_system_state.hpp"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdint>
#include <limits>

namespace blackbox::telemetry::macos {

MetricValue<bool> macos_low_power_mode() noexcept {
    @autoreleasepool {
        if (@available(macOS 12.0, *)) {
            NSProcessInfo* process_info = NSProcessInfo.processInfo;
            if (process_info == nil) {
                return MetricValue<bool>::unavailable(
                    MetricStatus::temporarily_unavailable);
            }
            return MetricValue<bool>::available(process_info.lowPowerModeEnabled != NO);
        }
    }
    return MetricValue<bool>::unavailable(MetricStatus::unsupported);
}

MetricValue<ProcessId> macos_frontmost_process_id() noexcept {
    @autoreleasepool {
        NSRunningApplication* application = NSWorkspace.sharedWorkspace.frontmostApplication;
        if (application == nil || application.processIdentifier <= 0) {
            return MetricValue<ProcessId>::unavailable(
                MetricStatus::temporarily_unavailable);
        }
        const auto pid = static_cast<std::uint64_t>(application.processIdentifier);
        if (pid > std::numeric_limits<std::uint32_t>::max()) {
            return MetricValue<ProcessId>::unavailable(
                MetricStatus::temporarily_unavailable);
        }
        return MetricValue<ProcessId>::available(
            ProcessId{static_cast<std::uint32_t>(pid)});
    }
}

GpuInventoryEvidence macos_gpu_inventory() noexcept {
    @autoreleasepool {
        NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
        if (devices == nil || devices.count >
                static_cast<NSUInteger>(std::numeric_limits<std::uint32_t>::max())) {
            return {};
        }
        std::uint32_t integrated{};
        std::uint32_t discrete{};
        for (id<MTLDevice> device in devices) {
            if (device == nil) continue;
            if (device.lowPower != NO) ++integrated;
            else ++discrete;
        }
        const auto count = integrated + discrete;
        GpuInventoryEvidence result{};
        result.device_count = MetricValue<std::uint32_t>::available(count);
        result.integrated_device_count =
            MetricValue<std::uint32_t>::available(integrated);
        result.discrete_device_count =
            MetricValue<std::uint32_t>::available(discrete);
        result.render_device_available = MetricValue<bool>::available(
            MTLCreateSystemDefaultDevice() != nil);
        return result;
    }
}

} // namespace blackbox::telemetry::macos
