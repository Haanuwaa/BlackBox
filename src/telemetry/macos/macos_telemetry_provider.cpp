#include "telemetry/macos/macos_telemetry_provider.hpp"

#include "telemetry/disk_quality_tracker.hpp"
#include "telemetry/io_counter_tracker.hpp"
#include "telemetry/macos/macos_process_collector.hpp"
#include "telemetry/macos/macos_system_state.hpp"
#include "telemetry/network_interface_tracker.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <ifaddrs.h>
#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <net/if.h>
#include <netinet/tcp_var.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <span>

namespace blackbox::telemetry::macos {
namespace {

template <typename T> [[nodiscard]] MetricValue<T> temporary() noexcept {
    return MetricValue<T>::unavailable(MetricStatus::temporarily_unavailable);
}

[[nodiscard]] bool add_ticks(const std::uint64_t left, const std::uint64_t right,
                             std::uint64_t& destination) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) return false;
    destination = left + right;
    return true;
}

[[nodiscard]] bool page_bytes(const std::uint64_t pages, const vm_size_t page_size,
                              std::uint64_t& destination) noexcept {
    if (page_size == 0U ||
        pages > std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(page_size)) {
        return false;
    }
    destination = pages * static_cast<std::uint64_t>(page_size);
    return true;
}

[[nodiscard]] bool dictionary_unsigned(const CFDictionaryRef dictionary, const CFStringRef key,
                                       std::uint64_t& destination) noexcept {
    const auto value = static_cast<CFTypeRef>(CFDictionaryGetValue(dictionary, key));
    if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) return false;
    std::int64_t signed_value{};
    if (CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &signed_value) ==
            0 ||
        signed_value < 0) {
        return false;
    }
    destination = static_cast<std::uint64_t>(signed_value);
    return true;
}

[[nodiscard]] MetricValue<std::uint32_t> processor_count(const char* name) noexcept {
    std::uint32_t value{};
    std::size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0U) != 0 || size != sizeof(value) ||
        value == 0U) {
        return temporary<std::uint32_t>();
    }
    return MetricValue<std::uint32_t>::available(value);
}

} // namespace

struct MacosTelemetryProvider::NativeState {
    static constexpr std::size_t maximum_network_interfaces = 128U;
    static constexpr std::size_t maximum_disk_devices = 128U;

    MacosMemoryPressureMonitor memory_pressure_monitor{};
    MacosSchedulerLatencyMonitor scheduler_latency_monitor{};

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
        if (!add_ticks(info.cpu_ticks[CPU_STATE_USER], info.cpu_ticks[CPU_STATE_SYSTEM],
                       user_system) ||
            !add_ticks(user_system, info.cpu_ticks[CPU_STATE_NICE], busy)) {
            return false;
        }
        std::uint64_t total{};
        if (!add_ticks(busy, info.cpu_ticks[CPU_STATE_IDLE], total) || total == 0U) {
            return false;
        }
        destination.system.cpu_time =
            MetricValue<CpuTimeCounters>::available(CpuTimeCounters{busy, total});

        std::uint32_t logical_processors{};
        std::size_t size = sizeof(logical_processors);
        if (sysctlbyname("hw.logicalcpu", &logical_processors, &size, nullptr, 0U) != 0 ||
            size != sizeof(logical_processors) || logical_processors == 0U) {
            host_basic_info_data_t basic{};
            mach_msg_type_number_t basic_count = HOST_BASIC_INFO_COUNT;
            if (host_info(mach_host_self(), HOST_BASIC_INFO, reinterpret_cast<host_info_t>(&basic),
                          &basic_count) == KERN_SUCCESS &&
                basic_count >= HOST_BASIC_INFO_COUNT && basic.avail_cpus > 0) {
                logical_processors = static_cast<std::uint32_t>(basic.avail_cpus);
            }
        }
        if (logical_processors != 0U) {
            destination.system.logical_processor_count =
                MetricValue<std::uint32_t>::available(logical_processors);
        }
        destination.system.physical_processor_count = processor_count("hw.physicalcpu");
        destination.system.active_processor_count = processor_count("hw.activecpu");
        return true;
    }

    [[nodiscard]] bool read_memory(RawTelemetrySnapshot& destination) noexcept {
        std::uint64_t total{};
        std::size_t total_size = sizeof(total);
        if (sysctlbyname("hw.memsize", &total, &total_size, nullptr, 0U) != 0 ||
            total_size != sizeof(total) || total == 0U) {
            host_basic_info_data_t basic{};
            mach_msg_type_number_t basic_count = HOST_BASIC_INFO_COUNT;
            if (host_info(mach_host_self(), HOST_BASIC_INFO, reinterpret_cast<host_info_t>(&basic),
                          &basic_count) != KERN_SUCCESS ||
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
            host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&statistics),
                              &count) != KERN_SUCCESS ||
            count < HOST_VM_INFO64_COUNT) {
            return false;
        }
        std::uint64_t available_pages{};
        if (!add_ticks(statistics.free_count, statistics.inactive_count, available_pages)) {
            return false;
        }
        std::uint64_t available{};
        if (!page_bytes(available_pages, page_size, available)) return false;
        if (available > total) available = total;
        destination.system.memory_total = MetricValue<ByteCount>::available(ByteCount{total});
        destination.system.memory_available =
            MetricValue<ByteCount>::available(ByteCount{available});

        auto& activity = destination.system.memory_activity;
        std::uint64_t compressed{};
        std::uint64_t page_out{};
        std::uint64_t swap_in{};
        std::uint64_t swap_out{};
        std::uint64_t compression{};
        std::uint64_t decompression{};
        if (!page_bytes(statistics.compressor_page_count, page_size, compressed) ||
            !page_bytes(statistics.pageouts, page_size, page_out) ||
            !page_bytes(statistics.swapins, page_size, swap_in) ||
            !page_bytes(statistics.swapouts, page_size, swap_out) ||
            !page_bytes(statistics.compressions, page_size, compression) ||
            !page_bytes(statistics.decompressions, page_size, decompression)) {
            return false;
        }
        if (compressed > total) compressed = total;
        activity.compressed_memory = MetricValue<ByteCount>::available(ByteCount{compressed});
        activity.page_out_bytes = MetricValue<ByteCount>::available(ByteCount{page_out});
        activity.swap_in_bytes = MetricValue<ByteCount>::available(ByteCount{swap_in});
        activity.swap_out_bytes = MetricValue<ByteCount>::available(ByteCount{swap_out});
        activity.compressed_bytes = MetricValue<ByteCount>::available(ByteCount{compression});
        activity.decompressed_bytes =
            MetricValue<ByteCount>::available(ByteCount{decompression});
        return true;
    }

    [[nodiscard]] bool read_network(RawTelemetrySnapshot& destination) noexcept {
        ifaddrs* addresses{};
        if (getifaddrs(&addresses) != 0 || addresses == nullptr) return false;

        std::size_t count{};
        bool valid = true;
        for (auto* address = addresses; address != nullptr; address = address->ifa_next) {
            if (address->ifa_addr == nullptr || address->ifa_data == nullptr ||
                address->ifa_name == nullptr || address->ifa_addr->sa_family != AF_LINK ||
                (address->ifa_flags & IFF_UP) == 0U || (address->ifa_flags & IFF_LOOPBACK) != 0U) {
                continue;
            }
            if (count == network_interfaces.size()) {
                valid = false;
                break;
            }
            const auto identity = if_nametoindex(address->ifa_name);
            if (identity == 0U) {
                valid = false;
                break;
            }
            const auto* data = static_cast<const if_data*>(address->ifa_data);
            network_interfaces[count++] = IoEntityCounters{
                static_cast<std::uint64_t>(identity), static_cast<std::uint64_t>(data->ifi_ibytes),
                static_cast<std::uint64_t>(data->ifi_obytes)};
            active_interface_ids[count - 1U] = static_cast<std::uint64_t>(identity);
        }
        freeifaddrs(addresses);
        if (!valid) return false;

        const auto aggregate = network_tracker.update(
            std::span<const IoEntityCounters>{network_interfaces.data(), count});
        destination.system.network_receive_bytes = aggregate.first;
        destination.system.network_transmit_bytes = aggregate.second;
        const auto state = interface_tracker.update(
            std::span<const std::uint64_t>{active_interface_ids.data(), count});
        if (!state) return false;
        destination.system.network_quality.connectivity =
            MetricValue<NetworkConnectivityLevel>::available(
                state->active_interfaces == 0U ? NetworkConnectivityLevel::disconnected
                                               : NetworkConnectivityLevel::local);
        destination.system.network_quality.active_interfaces =
            MetricValue<std::uint64_t>::available(state->active_interfaces);
        destination.system.network_quality.interface_change_counter =
            MetricValue<std::uint64_t>::available(state->change_counter);
        return aggregate.first.has_value() && aggregate.second.has_value();
    }

    [[nodiscard]] static bool read_tcp_quality(RawTelemetrySnapshot& destination) noexcept {
        tcpstat statistics{};
        std::size_t size = sizeof(statistics);
        if (sysctlbyname("net.inet.tcp.stats", &statistics, &size, nullptr, 0U) != 0 ||
            size < offsetof(tcpstat, tcps_sndrexmitpack) + sizeof(statistics.tcps_sndrexmitpack) ||
            statistics.tcps_sndtotal < statistics.tcps_sndrexmitpack) {
            return false;
        }
        std::uint64_t failures = statistics.tcps_conndrops;
        if (!add_ticks(failures, statistics.tcps_timeoutdrop, failures) ||
            !add_ticks(failures, statistics.tcps_persistdrop, failures) ||
            !add_ticks(failures, statistics.tcps_keepdrops, failures)) {
            return false;
        }
        auto& quality = destination.system.network_quality;
        quality.tcp_out_segments = MetricValue<std::uint64_t>::available(
            static_cast<std::uint64_t>(statistics.tcps_sndtotal) -
            static_cast<std::uint64_t>(statistics.tcps_sndrexmitpack));
        quality.tcp_retransmitted_segments =
            MetricValue<std::uint64_t>::available(statistics.tcps_sndrexmitpack);
        quality.tcp_failed_connections = MetricValue<std::uint64_t>::available(failures);
        quality.tcp_established_resets =
            MetricValue<std::uint64_t>::unavailable(MetricStatus::unsupported);
        return true;
    }

    [[nodiscard]] bool read_disks(RawTelemetrySnapshot& destination) noexcept {
        io_iterator_t iterator{};
        auto* matching = IOServiceMatching(kIOBlockStorageDriverClass);
        if (matching == nullptr ||
            IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS) {
            return false;
        }
        std::size_t count{};
        bool valid{true};
        while (const auto service = IOIteratorNext(iterator)) {
            if (count == disk_devices.size()) {
                IOObjectRelease(service);
                valid = false;
                break;
            }
            const auto property = IORegistryEntryCreateCFProperty(
                service, CFSTR(kIOBlockStorageDriverStatisticsKey), kCFAllocatorDefault, 0U);
            std::uint64_t identity{};
            std::uint64_t read_bytes{};
            std::uint64_t write_bytes{};
            std::uint64_t read_operations{};
            std::uint64_t write_operations{};
            std::uint64_t read_time_nanoseconds{};
            std::uint64_t write_time_nanoseconds{};
            const bool entry_valid =
                IORegistryEntryGetRegistryEntryID(service, &identity) == KERN_SUCCESS &&
                identity != 0U && property != nullptr &&
                CFGetTypeID(property) == CFDictionaryGetTypeID() &&
                dictionary_unsigned(static_cast<CFDictionaryRef>(property),
                                    CFSTR(kIOBlockStorageDriverStatisticsBytesReadKey),
                                    read_bytes) &&
                dictionary_unsigned(static_cast<CFDictionaryRef>(property),
                                    CFSTR(kIOBlockStorageDriverStatisticsBytesWrittenKey),
                                    write_bytes) &&
                dictionary_unsigned(static_cast<CFDictionaryRef>(property),
                                    CFSTR(kIOBlockStorageDriverStatisticsReadsKey),
                                    read_operations) &&
                dictionary_unsigned(static_cast<CFDictionaryRef>(property),
                                    CFSTR(kIOBlockStorageDriverStatisticsWritesKey),
                                    write_operations) &&
                dictionary_unsigned(static_cast<CFDictionaryRef>(property),
                                    CFSTR(kIOBlockStorageDriverStatisticsTotalReadTimeKey),
                                    read_time_nanoseconds) &&
                dictionary_unsigned(static_cast<CFDictionaryRef>(property),
                                    CFSTR(kIOBlockStorageDriverStatisticsTotalWriteTimeKey),
                                    write_time_nanoseconds);
            if (property != nullptr) CFRelease(property);
            IOObjectRelease(service);
            if (!entry_valid) continue;
            disk_devices[count] = IoEntityCounters{identity, read_bytes, write_bytes};
            disk_quality_counters[count] = DiskQualityCounters{identity,
                                                               read_operations,
                                                               write_operations,
                                                               read_time_nanoseconds,
                                                               write_time_nanoseconds,
                                                               std::nullopt};
            ++count;
        }
        IOObjectRelease(iterator);
        if (!valid || count == 0U) return false;
        const auto aggregate =
            disk_tracker.update(std::span<const IoEntityCounters>{disk_devices.data(), count});
        destination.system.disk_read_bytes = aggregate.first;
        destination.system.disk_write_bytes = aggregate.second;
        destination.system.disk_quality = disk_quality_tracker.update(
            destination.observed_at,
            std::span<const DiskQualityCounters>{disk_quality_counters.data(), count});
        return aggregate.first.has_value() && aggregate.second.has_value();
    }

    [[nodiscard]] bool read_power(RawTelemetrySnapshot& destination) noexcept {
        CFTypeRef snapshot = IOPSCopyPowerSourcesInfo();
        if (snapshot == nullptr) return false;

        const CFStringRef source = IOPSGetProvidingPowerSourceType(snapshot);
        if (source == nullptr) {
            CFRelease(snapshot);
            return false;
        }
        auto power_source = PowerSource::unknown;
        if (CFEqual(source, CFSTR(kIOPMACPowerKey)) != 0) {
            power_source = PowerSource::ac;
        } else if (CFEqual(source, CFSTR(kIOPMBatteryPowerKey)) != 0) {
            power_source = PowerSource::battery;
        } else if (CFEqual(source, CFSTR(kIOPMUPSPowerKey)) != 0) {
            power_source = PowerSource::ups_or_short_term;
        }
        destination.system.power_source = MetricValue<PowerSource>::available(power_source);
        destination.system.battery_saver = macos_low_power_mode();

        CFArrayRef sources = IOPSCopyPowerSourcesList(snapshot);
        if (sources != nullptr) {
            const CFIndex source_count = CFArrayGetCount(sources);
            for (CFIndex index = 0; index < source_count; ++index) {
                const CFTypeRef handle = CFArrayGetValueAtIndex(sources, index);
                const CFDictionaryRef description = IOPSGetPowerSourceDescription(snapshot, handle);
                if (description == nullptr) continue;
                const auto current = static_cast<CFNumberRef>(
                    CFDictionaryGetValue(description, CFSTR(kIOPSCurrentCapacityKey)));
                const auto maximum = static_cast<CFNumberRef>(
                    CFDictionaryGetValue(description, CFSTR(kIOPSMaxCapacityKey)));
                if (current == nullptr || maximum == nullptr ||
                    CFGetTypeID(current) != CFNumberGetTypeID() ||
                    CFGetTypeID(maximum) != CFNumberGetTypeID()) {
                    continue;
                }
                double current_value{};
                double maximum_value{};
                if (CFNumberGetValue(current, kCFNumberDoubleType, &current_value) != 0 &&
                    CFNumberGetValue(maximum, kCFNumberDoubleType, &maximum_value) != 0 &&
                    std::isfinite(current_value) && std::isfinite(maximum_value) &&
                    current_value >= 0.0 && maximum_value > 0.0) {
                    destination.system.battery_fraction = MetricValue<Ratio>::available(
                        Ratio{std::clamp(current_value / maximum_value, 0.0, 1.0)});
                    break;
                }
            }
            CFRelease(sources);
        }
        CFRelease(snapshot);
        return true;
    }

    [[nodiscard]] static bool read_uptime(RawTelemetrySnapshot& destination) noexcept {
        const auto nanoseconds = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        destination.system.system_uptime = MetricValue<Seconds>::available(
            Seconds{static_cast<double>(nanoseconds) / 1'000'000'000.0});
        return true;
    }

    MacosProcessCollector process_collector{};
    std::array<IoEntityCounters, maximum_network_interfaces> network_interfaces{};
    std::array<std::uint64_t, maximum_network_interfaces> active_interface_ids{};
    IoCounterTracker<maximum_network_interfaces> network_tracker{};
    NetworkInterfaceTracker<maximum_network_interfaces> interface_tracker{};
    std::array<IoEntityCounters, maximum_disk_devices> disk_devices{};
    std::array<DiskQualityCounters, maximum_disk_devices> disk_quality_counters{};
    IoCounterTracker<maximum_disk_devices> disk_tracker{};
    DiskQualityTracker<maximum_disk_devices> disk_quality_tracker{};
};

MacosTelemetryProvider::MacosTelemetryProvider(const core::IMonotonicClock& clock) noexcept
    : clock_{clock}, native_state_{new (std::nothrow) NativeState{}} {}

MacosTelemetryProvider::~MacosTelemetryProvider() = default;

ProviderSampleResult MacosTelemetryProvider::sample(const SamplingRequest request,
                                                    RawTelemetrySnapshot& destination) {
    destination.reset(clock_.now(), request.tiers);
    destination.system.network_receive_bytes = temporary<ByteCount>();
    destination.system.network_transmit_bytes = temporary<ByteCount>();
    destination.system.disk_read_bytes = temporary<ByteCount>();
    destination.system.disk_write_bytes = temporary<ByteCount>();
    destination.system.disk_quality.read_latency = temporary<Seconds>();
    destination.system.disk_quality.write_latency = temporary<Seconds>();
    destination.system.disk_quality.service_time = temporary<Seconds>();
    destination.system.disk_quality.service_concurrency = temporary<double>();
    destination.system.disk_quality.queue_depth =
        MetricValue<double>::unavailable(MetricStatus::unsupported);
    destination.system.disk_quality.worst_device_id = temporary<std::uint64_t>();
    destination.system.network_quality.connectivity = temporary<NetworkConnectivityLevel>();
    destination.system.network_quality.active_interfaces = temporary<std::uint64_t>();
    destination.system.network_quality.interface_change_counter = temporary<std::uint64_t>();
    destination.system.network_quality.tcp_out_segments = temporary<std::uint64_t>();
    destination.system.network_quality.tcp_retransmitted_segments = temporary<std::uint64_t>();
    destination.system.network_quality.tcp_failed_connections = temporary<std::uint64_t>();
    destination.system.network_quality.tcp_established_resets =
        MetricValue<std::uint64_t>::unavailable(MetricStatus::unsupported);
    destination.system.power_source = temporary<PowerSource>();
    destination.system.battery_fraction = temporary<Ratio>();
    destination.system.battery_saver = temporary<bool>();
    destination.system.system_uptime = temporary<Seconds>();
    destination.system.thermal_pressure_state = temporary<ThermalPressureState>();
    destination.system.memory_pressure_state =
        native_state_ != nullptr
            ? native_state_->memory_pressure_monitor.state()
            : temporary<MemoryPressureState>();
    destination.system.scheduler_delay =
        native_state_ != nullptr ? native_state_->scheduler_latency_monitor.state()
                                 : temporary<Seconds>();
    destination.system.foreground_process =
        request.collect_foreground_application
            ? temporary<ProcessIdentity>()
            : MetricValue<ProcessIdentity>::unavailable(MetricStatus::unsupported);
    destination.system.foreground_application =
        MetricValue<OpaqueApplicationIdentity>::unavailable(MetricStatus::unsupported);
    destination.system.foreground_gpu_usage =
        MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
    std::uint32_t attempted{};
    std::uint32_t failed{};

    if (request.tiers.contains(SamplingTier::fast)) {
        destination.system.cpu_time = temporary<CpuTimeCounters>();
        destination.system.logical_processor_count = temporary<std::uint32_t>();
        destination.system.physical_processor_count = temporary<std::uint32_t>();
        destination.system.active_processor_count = temporary<std::uint32_t>();
        ++attempted;
        if (native_state_ == nullptr || !native_state_->read_cpu(destination)) ++failed;
        ++attempted;
        if (native_state_ == nullptr || !native_state_->read_network(destination)) ++failed;
        ++attempted;
        if (native_state_ == nullptr || !native_state_->read_tcp_quality(destination)) ++failed;
        ++attempted;
        if (native_state_ == nullptr || !native_state_->read_disks(destination)) ++failed;
        ++attempted;
        if (native_state_ == nullptr || !native_state_->read_uptime(destination)) ++failed;
    }
    if (request.tiers.contains(SamplingTier::normal)) {
        destination.system.thermal_pressure_state = macos_thermal_pressure_state();
        const auto foreground_pid =
            request.collect_foreground_application
                ? macos_frontmost_process_id()
                : MetricValue<ProcessId>::unavailable(MetricStatus::unsupported);
        destination.system.memory_total = temporary<ByteCount>();
        destination.system.memory_available = temporary<ByteCount>();
        destination.system.memory_activity.compressed_memory = temporary<ByteCount>();
        destination.system.memory_activity.page_out_bytes = temporary<ByteCount>();
        destination.system.memory_activity.swap_in_bytes = temporary<ByteCount>();
        destination.system.memory_activity.swap_out_bytes = temporary<ByteCount>();
        destination.system.memory_activity.compressed_bytes = temporary<ByteCount>();
        destination.system.memory_activity.decompressed_bytes = temporary<ByteCount>();
        ++attempted;
        if (native_state_ == nullptr || !native_state_->read_memory(destination)) ++failed;
        ++attempted;
        if (native_state_ == nullptr || !native_state_->read_power(destination)) ++failed;
        ++attempted;
        if (native_state_ == nullptr || native_state_->process_collector.collect(
                                            true, request.tiers.contains(SamplingTier::slow),
                                            destination) != MetricStatus::available) {
            ++failed;
        }
        if (foreground_pid.has_value()) {
            const auto match =
                std::find_if(destination.processes.begin(), destination.processes.end(),
                             [pid = foreground_pid.value](const RawProcessCounters& process) {
                                 return process.identity.pid == pid;
                             });
            if (match != destination.processes.end()) {
                destination.system.foreground_process =
                    MetricValue<ProcessIdentity>::available(match->identity);
            }
        } else if (request.collect_foreground_application) {
            destination.system.foreground_process =
                MetricValue<ProcessIdentity>::unavailable(foreground_pid.status);
        }
    }

    const auto status = failed == 0U          ? ProviderSampleStatus::complete
                        : failed == attempted ? ProviderSampleStatus::temporarily_failed
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
    result.disk_throughput = true;
    result.disk_latency = true;
    result.disk_service_time = true;
    result.disk_service_concurrency = true;
    result.network_usage = true;
    result.network_connectivity = true;
    result.network_transport_quality = true;
    result.gpu_inventory = true;
    result.foreground_application = true;
    result.foreground_process_identity = true;
    result.power_status = true;
    result.system_uptime = true;
    result.thermal_pressure_state = true;
    result.memory_pressure_state = true;
    result.memory_activity = true;
    result.scheduler_responsiveness = true;
    result.cpu_topology = true;
    return result;
}

GpuInventoryEvidence MacosTelemetryProvider::gpu_inventory() const noexcept {
    return macos_gpu_inventory();
}

} // namespace blackbox::telemetry::macos
