#include "telemetry/linux/linux_telemetry_provider.hpp"

#include "telemetry/disk_quality_tracker.hpp"
#include "telemetry/io_counter_tracker.hpp"
#include "telemetry/linux/linux_gpu_collector.hpp"
#include "telemetry/linux/linux_proc_parser.hpp"
#include "telemetry/linux/linux_process_collector.hpp"
#include "telemetry/linux/linux_psi_parser.hpp"
#include "telemetry/network_interface_tracker.hpp"
#if defined(BLACKBOX_HAS_X11_FOREGROUND)
#include "telemetry/linux/linux_x11_foreground_reader.hpp"
#endif
#if defined(BLACKBOX_HAS_WAYLAND_FOREGROUND)
#include "telemetry/linux/linux_wayland_foreground_reader.hpp"
#endif

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>

namespace blackbox::telemetry::linux {
namespace {

constexpr std::size_t maximum_proc_file_bytes = 1024U * 1024U;
constexpr std::size_t maximum_io_entities = 128U;
constexpr std::size_t maximum_power_supplies = 32U;
constexpr std::size_t maximum_frequency_policies = 256U;

[[nodiscard]] bool read_bounded_proc_file(const char* path, std::string& destination) {
    std::ifstream input{path, std::ios::binary};
    if (!input) return false;
    destination.clear();
    std::array<char, 4096U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) break;
        if (destination.size() + static_cast<std::size_t>(count) > maximum_proc_file_bytes) {
            destination.clear();
            return false;
        }
        destination.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return input.eof() && !destination.empty();
}

template <typename T> [[nodiscard]] MetricValue<T> temporary() noexcept {
    return MetricValue<T>::unavailable(MetricStatus::temporarily_unavailable);
}

[[nodiscard]] constexpr std::uint64_t stable_identity(const std::string_view value) noexcept {
    std::uint64_t result{14695981039346656037ULL};
    for (const auto byte : value) {
        result ^= static_cast<unsigned char>(byte);
        result *= 1099511628211ULL;
    }
    return result;
}

} // namespace

struct LinuxTelemetryProvider::NativeState {
    NativeState() {
        system_contents.reserve(64U * 1024U);
        disk_contents.reserve(4U * 1024U);
        network_contents.reserve(64U * 1024U);
        tcp_contents.reserve(64U * 1024U);
        uptime_contents.reserve(256U);
        power_contents.reserve(16U * 1024U);
        frequency_contents.reserve(128U);
        frequency_max_contents.reserve(128U);
        frequency_cpu_contents.reserve(1U * 1024U);
        platform_profile_contents.reserve(128U);
        psi_cpu_contents.reserve(512U);
        psi_memory_contents.reserve(512U);
        psi_io_contents.reserve(512U);
    }

    [[nodiscard]] static MetricStatus read_pressure_file(const char* path, std::string& contents,
                                                         MetricValue<std::uint64_t>& some,
                                                         MetricValue<std::uint64_t>* full) {
        std::error_code error{};
        if (!std::filesystem::exists(path, error)) {
            const auto status = error ? MetricStatus::inaccessible : MetricStatus::unsupported;
            some = MetricValue<std::uint64_t>::unavailable(status);
            if (full != nullptr) *full = MetricValue<std::uint64_t>::unavailable(status);
            return status;
        }
        if (!read_bounded_proc_file(path, contents)) {
            some = MetricValue<std::uint64_t>::unavailable(MetricStatus::inaccessible);
            if (full != nullptr) {
                *full = MetricValue<std::uint64_t>::unavailable(MetricStatus::inaccessible);
            }
            return MetricStatus::inaccessible;
        }
        const auto parsed = parse_linux_psi(contents);
        if (!parsed) {
            some = temporary<std::uint64_t>();
            if (full != nullptr) *full = temporary<std::uint64_t>();
            return MetricStatus::temporarily_unavailable;
        }
        some = MetricValue<std::uint64_t>::available(parsed->some_total_microseconds);
        if (full != nullptr) {
            *full = parsed->full_total_microseconds
                        ? MetricValue<std::uint64_t>::available(*parsed->full_total_microseconds)
                        : temporary<std::uint64_t>();
        }
        return MetricStatus::available;
    }

    void read_pressure(RawTelemetrySnapshot& destination) {
        static_cast<void>(read_pressure_file("/proc/pressure/cpu", psi_cpu_contents,
                                             destination.system.pressure.cpu_some_microseconds,
                                             nullptr));
        static_cast<void>(
            read_pressure_file("/proc/pressure/memory", psi_memory_contents,
                               destination.system.pressure.memory_some_microseconds,
                               &destination.system.pressure.memory_full_microseconds));
        static_cast<void>(read_pressure_file("/proc/pressure/io", psi_io_contents,
                                             destination.system.pressure.io_some_microseconds,
                                             &destination.system.pressure.io_full_microseconds));
    }

    [[nodiscard]] MetricStatus read_disks(const core::MonotonicTimePoint observed_at) {
        std::error_code error{};
        std::size_t count{};
        const std::filesystem::path root{"/sys/block"};
        for (std::filesystem::directory_iterator iterator{root, error}, end;
             !error && iterator != end; iterator.increment(error)) {
            if (count == disks.size()) return MetricStatus::temporarily_unavailable;
            const auto device = iterator->path() / "device";
            if (!std::filesystem::exists(device, error)) {
                error.clear();
                continue;
            }
            const auto stat = iterator->path() / "stat";
            if (!read_bounded_proc_file(stat.c_str(), disk_contents)) {
                return MetricStatus::temporarily_unavailable;
            }
            const auto parsed = parse_sys_block_stat(disk_contents);
            if (!parsed) return MetricStatus::temporarily_unavailable;
            const auto name = iterator->path().filename().string();
            const auto identity = stable_identity(name);
            disks[count] = IoEntityCounters{identity, parsed->read_bytes, parsed->write_bytes};
            disk_quality_counters[count] = DiskQualityCounters{identity,
                                                               parsed->read_operations,
                                                               parsed->write_operations,
                                                               parsed->read_time_nanoseconds,
                                                               parsed->write_time_nanoseconds,
                                                               parsed->weighted_time_nanoseconds};
            ++count;
        }
        if (error || count == 0U) return MetricStatus::temporarily_unavailable;
        const auto totals =
            disk_tracker.update(std::span<const IoEntityCounters>{disks.data(), count});
        disk_read = totals.first;
        disk_write = totals.second;
        disk_quality = disk_quality_tracker.update(
            observed_at, std::span<const DiskQualityCounters>{disk_quality_counters.data(), count});
        return disk_read.status == MetricStatus::available &&
                       disk_write.status == MetricStatus::available
                   ? MetricStatus::available
                   : MetricStatus::temporarily_unavailable;
    }

    [[nodiscard]] MetricStatus read_network() {
        if (!read_bounded_proc_file("/proc/net/dev", network_contents)) {
            return MetricStatus::temporarily_unavailable;
        }
        const auto count = parse_proc_net_dev(network_contents, interfaces);
        if (!count) return MetricStatus::temporarily_unavailable;
        const auto totals =
            network_tracker.update(std::span<const IoEntityCounters>{interfaces.data(), *count});
        network_receive = totals.first;
        network_transmit = totals.second;
        return network_receive.status == MetricStatus::available &&
                       network_transmit.status == MetricStatus::available
                   ? MetricStatus::available
                   : MetricStatus::temporarily_unavailable;
    }

    [[nodiscard]] MetricStatus read_network_state(RawTelemetrySnapshot& destination) noexcept {
        ifaddrs* addresses{};
        if (getifaddrs(&addresses) != 0 || addresses == nullptr) {
            return MetricStatus::temporarily_unavailable;
        }
        std::size_t count{};
        bool valid{true};
        for (auto* address = addresses; address != nullptr; address = address->ifa_next) {
            if (address->ifa_addr == nullptr || address->ifa_name == nullptr ||
                address->ifa_addr->sa_family != AF_PACKET || (address->ifa_flags & IFF_UP) == 0U ||
                (address->ifa_flags & IFF_RUNNING) == 0U ||
                (address->ifa_flags & IFF_LOOPBACK) != 0U) {
                continue;
            }
            if (count == active_interface_ids.size()) {
                valid = false;
                break;
            }
            active_interface_ids[count++] = stable_identity(address->ifa_name);
        }
        freeifaddrs(addresses);
        if (!valid) return MetricStatus::temporarily_unavailable;
        const auto state = interface_tracker.update(
            std::span<const std::uint64_t>{active_interface_ids.data(), count});
        if (!state) return MetricStatus::temporarily_unavailable;
        destination.system.network_quality.connectivity =
            MetricValue<NetworkConnectivityLevel>::available(
                state->active_interfaces == 0U ? NetworkConnectivityLevel::disconnected
                                               : NetworkConnectivityLevel::local);
        destination.system.network_quality.active_interfaces =
            MetricValue<std::uint64_t>::available(state->active_interfaces);
        destination.system.network_quality.interface_change_counter =
            MetricValue<std::uint64_t>::available(state->change_counter);
        return MetricStatus::available;
    }

    [[nodiscard]] MetricStatus read_tcp_quality(RawTelemetrySnapshot& destination) {
        if (!read_bounded_proc_file("/proc/net/snmp", tcp_contents)) {
            return MetricStatus::temporarily_unavailable;
        }
        const auto parsed = parse_proc_net_snmp(tcp_contents);
        if (!parsed) return MetricStatus::temporarily_unavailable;
        auto& quality = destination.system.network_quality;
        quality.tcp_out_segments = MetricValue<std::uint64_t>::available(parsed->out_segments);
        quality.tcp_retransmitted_segments =
            MetricValue<std::uint64_t>::available(parsed->retransmitted_segments);
        quality.tcp_failed_connections =
            MetricValue<std::uint64_t>::available(parsed->failed_connections);
        quality.tcp_established_resets =
            MetricValue<std::uint64_t>::available(parsed->established_resets);
        return MetricStatus::available;
    }

    [[nodiscard]] MetricStatus read_uptime(RawTelemetrySnapshot& destination) {
        if (!read_bounded_proc_file("/proc/uptime", uptime_contents)) {
            return MetricStatus::temporarily_unavailable;
        }
        const auto parsed = parse_proc_uptime(uptime_contents);
        if (!parsed) return MetricStatus::temporarily_unavailable;
        destination.system.system_uptime = MetricValue<Seconds>::available(*parsed);
        return MetricStatus::available;
    }

    [[nodiscard]] MetricStatus read_power(RawTelemetrySnapshot& destination) {
        std::error_code error{};
        const std::filesystem::path root{"/sys/class/power_supply"};
        if (!std::filesystem::exists(root, error) || error) {
            return MetricStatus::temporarily_unavailable;
        }
        bool ac_online{};
        bool ups_online{};
        bool battery_present{};
        double battery_fraction_total{};
        std::uint32_t battery_fraction_count{};
        std::size_t supplies{};
        for (std::filesystem::directory_iterator iterator{root, error}, end;
             !error && iterator != end; iterator.increment(error)) {
            if (++supplies > maximum_power_supplies) {
                return MetricStatus::temporarily_unavailable;
            }
            if (!read_bounded_proc_file((iterator->path() / "uevent").c_str(), power_contents)) {
                return MetricStatus::temporarily_unavailable;
            }
            const auto parsed = parse_power_supply_uevent(power_contents);
            if (!parsed) return MetricStatus::temporarily_unavailable;
            if (!parsed->present) continue;
            if (parsed->kind == ProcPowerSupplyKind::mains) {
                ac_online = ac_online || parsed->online.value_or(false);
            } else if (parsed->kind == ProcPowerSupplyKind::ups) {
                ups_online = ups_online || parsed->online.value_or(false);
            } else if (parsed->kind == ProcPowerSupplyKind::battery) {
                battery_present = true;
                if (parsed->capacity_fraction) {
                    battery_fraction_total += parsed->capacity_fraction->value;
                    ++battery_fraction_count;
                }
            }
        }
        if (error) return MetricStatus::temporarily_unavailable;
        auto source = PowerSource::unknown;
        if (ac_online)
            source = PowerSource::ac;
        else if (ups_online)
            source = PowerSource::ups_or_short_term;
        else if (battery_present)
            source = PowerSource::battery;
        destination.system.power_source = MetricValue<PowerSource>::available(source);
        if (!battery_present) {
            destination.system.battery_fraction =
                MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
        } else if (battery_fraction_count != 0U) {
            destination.system.battery_fraction = MetricValue<Ratio>::available(
                Ratio{battery_fraction_total / static_cast<double>(battery_fraction_count)});
        }
        return MetricStatus::available;
    }

    [[nodiscard]] MetricStatus read_cpu_frequency(RawTelemetrySnapshot& destination) {
        std::error_code error{};
        const std::filesystem::path root{"/sys/devices/system/cpu/cpufreq"};
        if (!std::filesystem::exists(root, error) || error) {
            destination.system.cpu_current_mhz =
                MetricValue<double>::unavailable(MetricStatus::unsupported);
            destination.system.cpu_max_mhz =
                MetricValue<double>::unavailable(MetricStatus::unsupported);
            return MetricStatus::unsupported;
        }
        double weighted_current{};
        double weighted_maximum{};
        std::uint64_t cpu_count{};
        std::size_t policy_count{};
        for (std::filesystem::directory_iterator iterator{root, error}, end;
             !error && iterator != end; iterator.increment(error)) {
            const auto name = iterator->path().filename().string();
            if (!name.starts_with("policy")) continue;
            if (++policy_count > maximum_frequency_policies) {
                return MetricStatus::temporarily_unavailable;
            }
            if (!read_bounded_proc_file((iterator->path() / "scaling_cur_freq").c_str(),
                                        frequency_contents)) {
                return MetricStatus::temporarily_unavailable;
            }
            auto maximum_path = iterator->path() / "cpuinfo_max_freq";
            if (!read_bounded_proc_file(maximum_path.c_str(), frequency_max_contents)) {
                maximum_path = iterator->path() / "scaling_max_freq";
                if (!read_bounded_proc_file(maximum_path.c_str(), frequency_max_contents)) {
                    return MetricStatus::temporarily_unavailable;
                }
            }
            auto cpu_path = iterator->path() / "affected_cpus";
            if (!read_bounded_proc_file(cpu_path.c_str(), frequency_cpu_contents)) {
                cpu_path = iterator->path() / "related_cpus";
                if (!read_bounded_proc_file(cpu_path.c_str(), frequency_cpu_contents)) {
                    return MetricStatus::temporarily_unavailable;
                }
            }
            const auto current = parse_sysfs_frequency_mhz(frequency_contents);
            const auto maximum = parse_sysfs_frequency_mhz(frequency_max_contents);
            const auto policy_cpus = parse_sysfs_cpu_list_count(frequency_cpu_contents);
            if (!current || !maximum || !policy_cpus || *policy_cpus == 0U) {
                return MetricStatus::temporarily_unavailable;
            }
            weighted_current += *current * static_cast<double>(*policy_cpus);
            weighted_maximum += *maximum * static_cast<double>(*policy_cpus);
            cpu_count += *policy_cpus;
        }
        if (error) return MetricStatus::temporarily_unavailable;
        if (policy_count == 0U || cpu_count == 0U) {
            destination.system.cpu_current_mhz =
                MetricValue<double>::unavailable(MetricStatus::unsupported);
            destination.system.cpu_max_mhz =
                MetricValue<double>::unavailable(MetricStatus::unsupported);
            return MetricStatus::unsupported;
        }
        destination.system.cpu_current_mhz =
            MetricValue<double>::available(weighted_current / static_cast<double>(cpu_count));
        destination.system.cpu_max_mhz =
            MetricValue<double>::available(weighted_maximum / static_cast<double>(cpu_count));
        return MetricStatus::available;
    }

    [[nodiscard]] MetricStatus read_platform_profile(RawTelemetrySnapshot& destination) {
        const auto path = std::filesystem::path{"/sys/firmware/acpi/platform_profile"};
        std::error_code error{};
        if (!std::filesystem::exists(path, error) || error) {
            destination.system.battery_saver =
                MetricValue<bool>::unavailable(MetricStatus::unsupported);
            return MetricStatus::unsupported;
        }
        if (!read_bounded_proc_file(path.c_str(), platform_profile_contents)) {
            destination.system.battery_saver = temporary<bool>();
            return MetricStatus::temporarily_unavailable;
        }
        const auto saver = parse_linux_low_power_profile(platform_profile_contents);
        if (!saver) {
            destination.system.battery_saver = temporary<bool>();
            return MetricStatus::temporarily_unavailable;
        }
        destination.system.battery_saver = MetricValue<bool>::available(*saver);
        return MetricStatus::available;
    }

    std::array<IoEntityCounters, maximum_io_entities> disks{};
    std::array<DiskQualityCounters, maximum_io_entities> disk_quality_counters{};
    std::array<IoEntityCounters, maximum_io_entities> interfaces{};
    std::array<std::uint64_t, maximum_io_entities> active_interface_ids{};
    IoCounterTracker<maximum_io_entities> disk_tracker{};
    DiskQualityTracker<maximum_io_entities> disk_quality_tracker{};
    IoCounterTracker<maximum_io_entities> network_tracker{};
    NetworkInterfaceTracker<maximum_io_entities> interface_tracker{};
    LinuxProcessCollector process_collector{};
    LinuxGpuCollector gpu_collector{};
    MetricValue<ByteCount> disk_read{};
    MetricValue<ByteCount> disk_write{};
    RawDiskQuality disk_quality{};
    MetricValue<ByteCount> network_receive{};
    MetricValue<ByteCount> network_transmit{};
    std::string system_contents{};
    std::string disk_contents{};
    std::string network_contents{};
    std::string tcp_contents{};
    std::string uptime_contents{};
    std::string power_contents{};
    std::string frequency_contents{};
    std::string frequency_max_contents{};
    std::string frequency_cpu_contents{};
    std::string platform_profile_contents{};
    std::string psi_cpu_contents{};
    std::string psi_memory_contents{};
    std::string psi_io_contents{};
#if defined(BLACKBOX_HAS_X11_FOREGROUND)
    LinuxX11ForegroundReader foreground_reader{};
#endif
#if defined(BLACKBOX_HAS_WAYLAND_FOREGROUND)
    LinuxWaylandForegroundReader wayland_foreground_reader{};
#endif
};

LinuxTelemetryProvider::LinuxTelemetryProvider(const core::IMonotonicClock& clock) noexcept
    : clock_{clock}, native_state_{std::make_unique<NativeState>()} {}

LinuxTelemetryProvider::~LinuxTelemetryProvider() = default;

ProviderSampleResult LinuxTelemetryProvider::sample(const SamplingRequest request,
                                                    RawTelemetrySnapshot& destination) {
    destination.reset(clock_.now(), request.tiers);
    destination.system.cpu_time = temporary<CpuTimeCounters>();
    destination.system.logical_processor_count = temporary<std::uint32_t>();
    destination.system.memory_total = temporary<ByteCount>();
    destination.system.memory_available = temporary<ByteCount>();
    destination.system.disk_read_bytes = temporary<ByteCount>();
    destination.system.disk_write_bytes = temporary<ByteCount>();
    destination.system.disk_quality.read_latency = temporary<Seconds>();
    destination.system.disk_quality.write_latency = temporary<Seconds>();
    destination.system.disk_quality.service_time = temporary<Seconds>();
    destination.system.disk_quality.queue_depth = temporary<double>();
    destination.system.disk_quality.worst_device_id = temporary<std::uint64_t>();
    destination.system.network_receive_bytes = temporary<ByteCount>();
    destination.system.network_transmit_bytes = temporary<ByteCount>();
    destination.system.network_quality.connectivity = temporary<NetworkConnectivityLevel>();
    destination.system.network_quality.active_interfaces = temporary<std::uint64_t>();
    destination.system.network_quality.interface_change_counter = temporary<std::uint64_t>();
    destination.system.network_quality.tcp_out_segments = temporary<std::uint64_t>();
    destination.system.network_quality.tcp_retransmitted_segments = temporary<std::uint64_t>();
    destination.system.network_quality.tcp_failed_connections = temporary<std::uint64_t>();
    destination.system.network_quality.tcp_established_resets = temporary<std::uint64_t>();
    destination.system.power_source = temporary<PowerSource>();
    destination.system.battery_fraction = temporary<Ratio>();
    destination.system.battery_saver = MetricValue<bool>::unavailable(MetricStatus::unsupported);
    destination.system.system_uptime = temporary<Seconds>();
    destination.system.cpu_current_mhz = temporary<double>();
    destination.system.cpu_max_mhz = temporary<double>();
    destination.system.gpu_usage = MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
    destination.system.gpu_dedicated_memory =
        MetricValue<ByteCount>::unavailable(MetricStatus::unsupported);
    destination.system.gpu_shared_memory =
        MetricValue<ByteCount>::unavailable(MetricStatus::unsupported);
#if defined(BLACKBOX_HAS_X11_FOREGROUND)
    destination.system.foreground_process =
        request.collect_foreground_application
            ? temporary<ProcessIdentity>()
            : MetricValue<ProcessIdentity>::unavailable(MetricStatus::unsupported);
#else
    destination.system.foreground_process =
        MetricValue<ProcessIdentity>::unavailable(MetricStatus::unsupported);
#endif
#if defined(BLACKBOX_HAS_WAYLAND_FOREGROUND)
    destination.system.foreground_application =
        request.collect_foreground_application
            ? temporary<OpaqueApplicationIdentity>()
            : MetricValue<OpaqueApplicationIdentity>::unavailable(MetricStatus::unsupported);
#else
    destination.system.foreground_application =
        MetricValue<OpaqueApplicationIdentity>::unavailable(MetricStatus::unsupported);
#endif
    destination.system.foreground_gpu_usage =
        MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
    std::uint32_t attempted{};
    std::uint32_t failed{};
    auto& contents = native_state_->system_contents;

    if (request.tiers.contains(SamplingTier::fast)) {
        if (native_state_ != nullptr) native_state_->read_pressure(destination);
        ++attempted;
        if (read_bounded_proc_file("/proc/stat", contents)) {
            const auto parsed = parse_proc_stat(contents);
            if (parsed) {
                destination.system.cpu_time =
                    MetricValue<CpuTimeCounters>::available(parsed->counters);
                destination.system.logical_processor_count =
                    MetricValue<std::uint32_t>::available(parsed->logical_processor_count);
            } else {
                ++failed;
            }
        } else {
            ++failed;
        }
        ++attempted;
        if (native_state_ == nullptr || native_state_->read_cpu_frequency(destination) ==
                                            MetricStatus::temporarily_unavailable) {
            ++failed;
        }
        ++attempted;
        if (native_state_ != nullptr &&
            native_state_->read_disks(destination.observed_at) == MetricStatus::available) {
            destination.system.disk_read_bytes = native_state_->disk_read;
            destination.system.disk_write_bytes = native_state_->disk_write;
            destination.system.disk_quality = native_state_->disk_quality;
        } else {
            ++failed;
        }
        ++attempted;
        if (native_state_ != nullptr && native_state_->read_network() == MetricStatus::available) {
            destination.system.network_receive_bytes = native_state_->network_receive;
            destination.system.network_transmit_bytes = native_state_->network_transmit;
        } else {
            ++failed;
        }
        ++attempted;
        if (native_state_ == nullptr ||
            native_state_->read_network_state(destination) != MetricStatus::available) {
            ++failed;
        }
        ++attempted;
        if (native_state_ == nullptr ||
            native_state_->read_tcp_quality(destination) != MetricStatus::available) {
            ++failed;
        }
        ++attempted;
        if (native_state_ == nullptr ||
            native_state_->read_uptime(destination) != MetricStatus::available) {
            ++failed;
        }
    }

    if (request.tiers.contains(SamplingTier::normal)) {
        std::optional<ProcessIdentity> foreground_identity{};
#if defined(BLACKBOX_HAS_X11_FOREGROUND)
        const auto foreground_pid =
            request.collect_foreground_application && native_state_ != nullptr
                ? native_state_->foreground_reader.read()
                : MetricValue<ProcessId>::unavailable(MetricStatus::unsupported);
#endif
#if defined(BLACKBOX_HAS_WAYLAND_FOREGROUND)
        if (request.collect_foreground_application && native_state_ != nullptr) {
            destination.system.foreground_application =
                native_state_->wayland_foreground_reader.read();
        }
#endif
        ++attempted;
        if (read_bounded_proc_file("/proc/meminfo", contents)) {
            const auto parsed = parse_proc_meminfo(contents);
            if (parsed) {
                destination.system.memory_total = MetricValue<ByteCount>::available(parsed->total);
                destination.system.memory_available =
                    MetricValue<ByteCount>::available(parsed->available);
            } else {
                ++failed;
            }
        } else {
            ++failed;
        }
        ++attempted;
        if (native_state_ == nullptr ||
            native_state_->read_power(destination) != MetricStatus::available) {
            ++failed;
        }
        ++attempted;
        if (native_state_ == nullptr || native_state_->read_platform_profile(destination) ==
                                            MetricStatus::temporarily_unavailable) {
            ++failed;
        }
        ++attempted;
        if (native_state_ == nullptr ||
            native_state_->process_collector.collect(request.tiers.contains(SamplingTier::slow),
                                                     destination) != MetricStatus::available) {
            ++failed;
        }
#if defined(BLACKBOX_HAS_X11_FOREGROUND)
        if (foreground_pid.has_value()) {
            const auto match =
                std::find_if(destination.processes.begin(), destination.processes.end(),
                             [pid = foreground_pid.value](const RawProcessCounters& process) {
                                 return process.identity.pid == pid;
                             });
            if (match != destination.processes.end()) {
                destination.system.foreground_process =
                    MetricValue<ProcessIdentity>::available(match->identity);
                foreground_identity = match->identity;
            }
        } else if (request.collect_foreground_application) {
            destination.system.foreground_process =
                MetricValue<ProcessIdentity>::unavailable(foreground_pid.status);
        }
#endif
        const auto gpu =
            native_state_->gpu_collector.collect(destination.observed_at, foreground_identity,
                                                 request.tiers.contains(SamplingTier::slow));
        destination.system.gpu_usage = gpu.system.busiest_engine_usage;
        destination.system.gpu_dedicated_memory = gpu.system.dedicated_memory_used;
        destination.system.foreground_gpu_usage = gpu.foreground_usage;
        if (native_state_->gpu_collector.supports_system_usage()) {
            ++attempted;
            if (!destination.system.gpu_usage.has_value()) ++failed;
        }
        if (foreground_identity && native_state_->gpu_collector.supports_foreground_usage()) {
            ++attempted;
            if (!destination.system.foreground_gpu_usage.has_value()) ++failed;
        }
    }

    const auto status = failed == 0U          ? ProviderSampleStatus::complete
                        : failed == attempted ? ProviderSampleStatus::temporarily_failed
                                              : ProviderSampleStatus::partial;
    return ProviderSampleResult{status, ++sequence_};
}

PlatformCapabilities LinuxTelemetryProvider::capabilities() const noexcept {
    PlatformCapabilities result{};
    result.cpu_usage = true;
    result.memory_usage = true;
    result.process_cpu = true;
    result.process_memory = true;
    result.process_disk_io = true;
    result.disk_throughput = true;
    result.disk_latency = true;
    result.disk_queue_depth = true;
    result.disk_service_time = true;
    result.network_usage = true;
    result.network_connectivity = true;
    result.network_transport_quality = true;
    result.gpu_usage =
        native_state_ != nullptr && native_state_->gpu_collector.supports_system_usage();
    result.gpu_memory = native_state_ != nullptr && native_state_->gpu_collector.supports_memory();
    result.gpu_inventory = native_state_ != nullptr;
    result.foreground_gpu_usage =
        native_state_ != nullptr && native_state_->gpu_collector.supports_foreground_usage();
    result.cpu_frequency = true;
#if defined(BLACKBOX_HAS_X11_FOREGROUND)
    result.foreground_process_identity =
        native_state_ != nullptr &&
        native_state_->foreground_reader.status() != MetricStatus::unsupported;
#endif
#if defined(BLACKBOX_HAS_WAYLAND_FOREGROUND)
    result.foreground_application_identity =
        native_state_ != nullptr && native_state_->wayland_foreground_reader.candidate();
#endif
    result.foreground_application =
        result.foreground_process_identity || result.foreground_application_identity;
    result.power_status = true;
    result.system_uptime = true;
    result.cpu_some_pressure = true;
    result.memory_some_pressure = true;
    result.memory_full_pressure = true;
    result.io_some_pressure = true;
    result.io_full_pressure = true;
    return result;
}

GpuInventoryEvidence LinuxTelemetryProvider::gpu_inventory() const noexcept {
    return native_state_ != nullptr ? native_state_->gpu_collector.inventory()
                                    : GpuInventoryEvidence{};
}

} // namespace blackbox::telemetry::linux
