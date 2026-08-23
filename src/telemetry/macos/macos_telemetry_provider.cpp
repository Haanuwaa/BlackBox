#include "telemetry/macos/macos_telemetry_provider.hpp"

#include "telemetry/macos/macos_process_collector.hpp"

#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <sys/sysctl.h>

#include <cstdint>
#include <limits>
#include <new>

namespace blackbox::telemetry::macos {
namespace {

template <typename T>
[[nodiscard]] MetricValue<T> temporary() noexcept {
    return MetricValue<T>::unavailable(MetricStatus::temporarily_unavailable);
}

[[nodiscard]] bool add_ticks(const std::uint64_t left,
                             const std::uint64_t right,
                             std::uint64_t& destination) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) return false;
    destination = left + right;
    return true;
}

[[nodiscard]] bool page_bytes(const std::uint64_t pages,
                              const vm_size_t page_size,
                              std::uint64_t& destination) noexcept {
    if (page_size == 0U ||
        pages > std::numeric_limits<std::uint64_t>::max() /
                    static_cast<std::uint64_t>(page_size)) {
        return false;
    }
    destination = pages * static_cast<std::uint64_t>(page_size);
    return true;
}

} // namespace

struct MacosTelemetryProvider::NativeState {
    [[nodiscard]] bool read_cpu(RawTelemetrySnapshot& destination) noexcept {
        host_cpu_load_info_data_t info{};
        mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
        if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                            reinterpret_cast<host_info_t>(&info), &count) != KERN_SUCCESS ||
            count != HOST_CPU_LOAD_INFO_COUNT) {
            return false;
        }
        std::uint64_t busy{};
        std::uint64_t user_system{};
        if (!add_ticks(info.cpu_ticks[CPU_STATE_USER],
                       info.cpu_ticks[CPU_STATE_SYSTEM], user_system) ||
            !add_ticks(user_system, info.cpu_ticks[CPU_STATE_NICE], busy)) {
            return false;
        }
        std::uint64_t total{};
        if (!add_ticks(busy, info.cpu_ticks[CPU_STATE_IDLE], total) || total == 0U) {
            return false;
        }
        destination.system.cpu_time = MetricValue<CpuTimeCounters>::available(
            CpuTimeCounters{busy, total});

        std::uint32_t logical_processors{};
        std::size_t size = sizeof(logical_processors);
        if (sysctlbyname("hw.logicalcpu", &logical_processors, &size, nullptr, 0U) != 0 ||
            size != sizeof(logical_processors) || logical_processors == 0U) {
            host_basic_info_data_t basic{};
            mach_msg_type_number_t basic_count = HOST_BASIC_INFO_COUNT;
            if (host_info(mach_host_self(), HOST_BASIC_INFO,
                          reinterpret_cast<host_info_t>(&basic), &basic_count) == KERN_SUCCESS &&
                basic_count >= HOST_BASIC_INFO_COUNT && basic.avail_cpus > 0) {
                logical_processors = static_cast<std::uint32_t>(basic.avail_cpus);
            }
        }
        if (logical_processors != 0U) {
            destination.system.logical_processor_count =
                MetricValue<std::uint32_t>::available(logical_processors);
        }
        return true;
    }

    [[nodiscard]] bool read_memory(RawTelemetrySnapshot& destination) noexcept {
        std::uint64_t total{};
        std::size_t total_size = sizeof(total);
        if (sysctlbyname("hw.memsize", &total, &total_size, nullptr, 0U) != 0 ||
            total_size != sizeof(total) || total == 0U) {
            host_basic_info_data_t basic{};
            mach_msg_type_number_t basic_count = HOST_BASIC_INFO_COUNT;
            if (host_info(mach_host_self(), HOST_BASIC_INFO,
                          reinterpret_cast<host_info_t>(&basic), &basic_count) != KERN_SUCCESS ||
                basic_count < HOST_BASIC_INFO_COUNT || basic.max_mem == 0U) {
                return false;
            }
            total = static_cast<std::uint64_t>(basic.max_mem);
        }

        vm_statistics64_data_t statistics{};
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        mach_port_t host = mach_host_self();
        vm_size_t page_size{};
        if (host_page_size(host, &page_size) != KERN_SUCCESS ||
            host_statistics64(host, HOST_VM_INFO64,
                              reinterpret_cast<host_info64_t>(&statistics), &count) !=
                KERN_SUCCESS ||
            count < HOST_VM_INFO64_REV0_COUNT) {
            return false;
        }
        std::uint64_t available_pages{};
        if (!add_ticks(statistics.free_count, statistics.inactive_count,
                       available_pages)) {
            return false;
        }
        std::uint64_t available{};
        if (!page_bytes(available_pages, page_size, available)) return false;
        if (available > total) available = total;
        destination.system.memory_total =
            MetricValue<ByteCount>::available(ByteCount{total});
        destination.system.memory_available =
            MetricValue<ByteCount>::available(ByteCount{available});
        return true;
    }

    MacosProcessCollector process_collector{};
};

MacosTelemetryProvider::MacosTelemetryProvider(
    const core::IMonotonicClock& clock) noexcept
    : clock_{clock}, native_state_{new (std::nothrow) NativeState{}} {}

MacosTelemetryProvider::~MacosTelemetryProvider() = default;

ProviderSampleResult MacosTelemetryProvider::sample(
    const SamplingRequest request,
    RawTelemetrySnapshot& destination) {
    destination.reset(clock_.now(), request.tiers);
    std::uint32_t attempted{};
    std::uint32_t failed{};

    if (request.tiers.contains(SamplingTier::fast)) {
        destination.system.cpu_time = temporary<CpuTimeCounters>();
        destination.system.logical_processor_count = temporary<std::uint32_t>();
        ++attempted;
        if (native_state_ == nullptr || !native_state_->read_cpu(destination)) ++failed;
    }
    if (request.tiers.contains(SamplingTier::normal)) {
        destination.system.memory_total = temporary<ByteCount>();
        destination.system.memory_available = temporary<ByteCount>();
        ++attempted;
        if (native_state_ == nullptr || !native_state_->read_memory(destination)) ++failed;
        ++attempted;
        if (native_state_ == nullptr ||
            native_state_->process_collector.collect(
                true, request.tiers.contains(SamplingTier::slow), destination) !=
                MetricStatus::available) {
            ++failed;
        }
    }

    const auto status = failed == 0U ? ProviderSampleStatus::complete
                        : failed == attempted
                            ? ProviderSampleStatus::temporarily_failed
                            : ProviderSampleStatus::partial;
    return ProviderSampleResult{status, ++sequence_};
}

PlatformCapabilities MacosTelemetryProvider::capabilities() const noexcept {
    PlatformCapabilities result{};
    result.cpu_usage = true;
    result.memory_usage = true;
    result.process_cpu = true;
    result.process_memory = true;
    result.process_disk_io = true;
    return result;
}

} // namespace blackbox::telemetry::macos
