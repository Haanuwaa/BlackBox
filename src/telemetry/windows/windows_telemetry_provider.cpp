#include "telemetry/windows/windows_telemetry_provider.hpp"

#include "telemetry/windows/windows_counter_conversion.hpp"
#include "telemetry/windows/windows_process_collector.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <dxgi1_2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <winuser.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <mutex>
#include <span>

namespace blackbox::telemetry::windows {
namespace {

// The Windows SDK intentionally omits this public documentation structure on
// some SDK revisions. Keep the exact documented ABI local to the Win32 backend.
struct ProcessorPowerInformation {
    ULONG number{};
    ULONG max_mhz{};
    ULONG current_mhz{};
    ULONG mhz_limit{};
    ULONG max_idle_state{};
    ULONG current_idle_state{};
};

[[nodiscard]] constexpr std::uint64_t file_time_to_ticks(const FILETIME value) noexcept {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
           static_cast<std::uint64_t>(value.dwLowDateTime);
}

[[nodiscard]] bool prepare_sampling_thread() noexcept {
    const auto thread = GetCurrentThread();
    const bool priority_prepared =
        SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST) != FALSE;

    // Windows may apply EcoQoS execution-speed throttling after a process has
    // remained hidden for a long time. The one-second sampler is already
    // bounded and extremely low duty-cycle, so opt only this worker out while
    // retaining the highest non-real-time priority contract. Older systems may
    // reject the hint; priority preparation remains the required capability.
    THREAD_POWER_THROTTLING_STATE throttling{};
    throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0U;
    static_cast<void>(SetThreadInformation(
        thread, ThreadPowerThrottling, &throttling,
        static_cast<DWORD>(sizeof(throttling))));
    return priority_prepared;
}

template <typename T>
[[nodiscard]] MetricValue<T> temporary() noexcept {
    return MetricValue<T>::unavailable(MetricStatus::temporarily_unavailable);
}

[[nodiscard]] bool read_system_times(WindowsSystemTimes& result) noexcept {
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user) == 0) {
        return false;
    }
    result.idle_ticks = file_time_to_ticks(idle);
    result.kernel_ticks_including_idle = file_time_to_ticks(kernel);
    result.user_ticks = file_time_to_ticks(user);
    return true;
}

[[nodiscard]] bool read_physical_memory(WindowsPhysicalMemory& result) noexcept {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory) == 0) {
        return false;
    }
    result.total_bytes = memory.ullTotalPhys;
    result.available_bytes = memory.ullAvailPhys;
    return true;
}

[[nodiscard]] bool valid_pdh_counter(const PDH_RAW_COUNTER& counter) noexcept {
    return counter.CStatus == PDH_CSTATUS_VALID_DATA ||
           counter.CStatus == PDH_CSTATUS_NEW_DATA;
}

[[nodiscard]] bool disk_identity(const wchar_t* name,
                                 std::uint64_t& identity) noexcept {
    if (name == nullptr || std::wcscmp(name, L"_Total") == 0) {
        return false;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const auto number = std::wcstoull(name, &end, 10);
    if (errno != 0 || end == name || (*end != L' ' && *end != L'\0') ||
        number == UINT64_MAX) {
        return false;
    }
    identity = static_cast<std::uint64_t>(number) + 1U;
    return true;
}

[[nodiscard]] MetricStatus pdh_error_status(const PDH_STATUS status) noexcept {
    return status == PDH_ACCESS_DENIED
               ? MetricStatus::inaccessible
               : MetricStatus::temporarily_unavailable;
}

} // namespace

struct WindowsTelemetryProvider::NativeState {
    NativeState() noexcept {
        initialize_gpu_inventory();
        if (PdhOpenQueryW(nullptr, 0U, &fast_query) != ERROR_SUCCESS) {
            initialize_network_state();
            return;
        }
        const auto disk_added = PdhAddEnglishCounterW(
                fast_query, L"\\PhysicalDisk(*)\\Disk Read Bytes/sec", 0U,
                &disk_read_counter) == ERROR_SUCCESS &&
            PdhAddEnglishCounterW(
                fast_query, L"\\PhysicalDisk(*)\\Disk Write Bytes/sec", 0U,
                &disk_write_counter) == ERROR_SUCCESS;
        if (!disk_added) {
            disk_read_counter = nullptr;
            disk_write_counter = nullptr;
        }
        const auto disk_quality_added =
                PdhAddEnglishCounterW(fast_query,
                    L"\\PhysicalDisk(*)\\Avg. Disk sec/Read", 0U,
                    &disk_read_latency_counter) == ERROR_SUCCESS &&
                PdhAddEnglishCounterW(fast_query,
                    L"\\PhysicalDisk(*)\\Avg. Disk sec/Write", 0U,
                    &disk_write_latency_counter) == ERROR_SUCCESS &&
                PdhAddEnglishCounterW(fast_query,
                    L"\\PhysicalDisk(*)\\Avg. Disk sec/Transfer", 0U,
                    &disk_service_time_counter) == ERROR_SUCCESS &&
                PdhAddEnglishCounterW(fast_query,
                    L"\\PhysicalDisk(*)\\Current Disk Queue Length", 0U,
                    &disk_queue_counter) == ERROR_SUCCESS;
        if (!disk_quality_added) {
            disk_read_latency_counter = nullptr;
            disk_write_latency_counter = nullptr;
            disk_service_time_counter = nullptr;
            disk_queue_counter = nullptr;
        }
        const auto gpu_added =
                PdhAddEnglishCounterW(fast_query,
                    L"\\GPU Engine(*)\\Utilization Percentage", 0U,
                    &gpu_usage_counter) == ERROR_SUCCESS &&
                PdhAddEnglishCounterW(fast_query,
                    L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0U,
                    &gpu_dedicated_counter) == ERROR_SUCCESS &&
                PdhAddEnglishCounterW(fast_query,
                    L"\\GPU Adapter Memory(*)\\Shared Usage", 0U,
                    &gpu_shared_counter) == ERROR_SUCCESS;
        if (!gpu_added) {
            gpu_usage_counter = nullptr;
            gpu_dedicated_counter = nullptr;
            gpu_shared_counter = nullptr;
        }
        const auto responsiveness_added =
                PdhAddEnglishCounterW(fast_query,
                    L"\\Processor Information(_Total)\\% DPC Time", 0U,
                    &dpc_time_counter) == ERROR_SUCCESS &&
                PdhAddEnglishCounterW(fast_query,
                    L"\\Processor Information(_Total)\\% Interrupt Time", 0U,
                    &interrupt_time_counter) == ERROR_SUCCESS &&
                PdhAddEnglishCounterW(fast_query,
                    L"\\Processor Information(_Total)\\DPC Rate", 0U,
                    &dpc_rate_counter) == ERROR_SUCCESS;
        if (!responsiveness_added) {
            dpc_time_counter = nullptr;
            interrupt_time_counter = nullptr;
            dpc_rate_counter = nullptr;
        }
        if (PdhCollectQueryData(fast_query) != ERROR_SUCCESS) {
            PdhCloseQuery(fast_query);
            fast_query = nullptr;
            disk_read_counter = nullptr;
            disk_write_counter = nullptr;
            disk_read_latency_counter = nullptr;
            disk_write_latency_counter = nullptr;
            disk_service_time_counter = nullptr;
            disk_queue_counter = nullptr;
            gpu_usage_counter = nullptr;
            gpu_dedicated_counter = nullptr;
            gpu_shared_counter = nullptr;
            dpc_time_counter = nullptr;
            interrupt_time_counter = nullptr;
            dpc_rate_counter = nullptr;
        }
        initialize_network_state();
    }

    ~NativeState() {
        if (interface_notification != nullptr) {
            static_cast<void>(CancelMibChangeNotify2(interface_notification));
        }
        if (connectivity_notification != nullptr) {
            static_cast<void>(CancelMibChangeNotify2(connectivity_notification));
        }
        if (fast_query != nullptr) {
            PdhCloseQuery(fast_query);
        }
    }

    NativeState(const NativeState&) = delete;
    NativeState& operator=(const NativeState&) = delete;

    void initialize_gpu_inventory() noexcept {
        auto unavailable = [this]() noexcept {
            const auto status = MetricStatus::temporarily_unavailable;
            gpu_inventory_evidence.device_count =
                MetricValue<std::uint32_t>::unavailable(status);
            gpu_inventory_evidence.integrated_device_count =
                MetricValue<std::uint32_t>::unavailable(status);
            gpu_inventory_evidence.discrete_device_count =
                MetricValue<std::uint32_t>::unavailable(status);
            gpu_inventory_evidence.unknown_device_count =
                MetricValue<std::uint32_t>::unavailable(status);
            gpu_inventory_evidence.render_device_available =
                MetricValue<bool>::unavailable(status);
        };
        unavailable();
        IDXGIFactory1* factory{};
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                      reinterpret_cast<void**>(&factory))) ||
            factory == nullptr) {
            return;
        }
        std::uint32_t hardware_count{};
        bool failed{};
        bool enumeration_complete{};
        for (UINT index = 0U; index < 256U; ++index) {
            IDXGIAdapter1* adapter{};
            const auto result = factory->EnumAdapters1(index, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND) {
                enumeration_complete = true;
                break;
            }
            if (FAILED(result) || adapter == nullptr) {
                failed = true;
                break;
            }
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(adapter->GetDesc1(&description))) {
                failed = true;
            } else if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0U) {
                ++hardware_count;
            }
            adapter->Release();
            if (failed) break;
        }
        factory->Release();
        if (failed || !enumeration_complete) return;
        gpu_inventory_evidence.device_count =
            MetricValue<std::uint32_t>::available(hardware_count);
        gpu_inventory_evidence.integrated_device_count =
            MetricValue<std::uint32_t>::available(0U);
        gpu_inventory_evidence.discrete_device_count =
            MetricValue<std::uint32_t>::available(0U);
        gpu_inventory_evidence.unknown_device_count =
            MetricValue<std::uint32_t>::available(hardware_count);
        gpu_inventory_evidence.render_device_available =
            MetricValue<bool>::available(hardware_count != 0U);
    }

    static VOID NETIOAPI_API_ connectivity_changed(
        void* context,
        const NL_NETWORK_CONNECTIVITY_HINT hint) noexcept {
        if (context == nullptr) {
            return;
        }
        auto& state = *static_cast<NativeState*>(context);
        auto connectivity = NetworkConnectivityLevel::unknown;
        switch (hint.ConnectivityLevel) {
        case NetworkConnectivityLevelHintNone:
            connectivity = NetworkConnectivityLevel::disconnected;
            break;
        case NetworkConnectivityLevelHintLocalAccess:
            connectivity = NetworkConnectivityLevel::local;
            break;
        case NetworkConnectivityLevelHintInternetAccess:
            connectivity = NetworkConnectivityLevel::internet;
            break;
        case NetworkConnectivityLevelHintConstrainedInternetAccess:
            connectivity = NetworkConnectivityLevel::constrained;
            break;
        default: break;
        }
        state.connectivity_hint.store(connectivity, std::memory_order_relaxed);
        state.connectivity_hint_ready.store(true, std::memory_order_release);
    }

    static VOID NETIOAPI_API_ interfaces_changed(
        void* context,
        MIB_IPINTERFACE_ROW*,
        MIB_NOTIFICATION_TYPE) noexcept {
        if (context != nullptr) {
            static_cast<NativeState*>(context)->refresh_interface_inventory();
        }
    }

    void refresh_interface_inventory() noexcept {
        PMIB_IF_TABLE2 table = nullptr;
        if (GetIfTable2(&table) != NO_ERROR) {
            return;
        }
        std::array<std::uint64_t, maximum_tracked_interfaces> refreshed{};
        std::size_t refreshed_count{};
        bool refreshed_overflow{};
        for (ULONG index = 0U; index < table->NumEntries; ++index) {
            const auto& row = table->Table[index];
            const auto flags = row.InterfaceAndOperStatusFlags;
            const bool eligible = flags.HardwareInterface != 0U &&
                                  flags.FilterInterface == 0U &&
                                  flags.EndPointInterface == 0U &&
                                  row.Type != IF_TYPE_SOFTWARE_LOOPBACK &&
                                  row.Type != IF_TYPE_TUNNEL;
            if (!eligible) {
                continue;
            }
            const auto identity = row.InterfaceLuid.Value;
            const auto refreshed_end = refreshed.begin() +
                                       static_cast<std::ptrdiff_t>(refreshed_count);
            if (std::find(refreshed.begin(), refreshed_end, identity) != refreshed_end) {
                continue;
            }
            if (refreshed_count >= refreshed.size()) {
                refreshed_overflow = true;
                continue;
            }
            refreshed[refreshed_count++] = identity;
        }
        FreeMibTable(table);
        const std::scoped_lock lock{interface_inventory_mutex};
        interface_inventory = refreshed;
        interface_inventory_count = refreshed_count;
        interface_inventory_overflow = refreshed_overflow;
    }

    void initialize_network_state() noexcept {
        refresh_interface_inventory();
        if (NotifyIpInterfaceChange(
                AF_UNSPEC, interfaces_changed, this, FALSE,
                &interface_notification) != NO_ERROR) {
            interface_notification = nullptr;
        }
        if (NotifyNetworkConnectivityHintChange(
                connectivity_changed, this, TRUE,
                &connectivity_notification) != NO_ERROR) {
            connectivity_notification = nullptr;
        }
    }

    [[nodiscard]] MetricStatus collect_fast() noexcept {
        if (fast_query == nullptr) {
            fast_collection_status = MetricStatus::temporarily_unavailable;
        } else {
            const auto status = PdhCollectQueryData(fast_query);
            fast_collection_status = status == ERROR_SUCCESS
                                         ? MetricStatus::available
                                         : pdh_error_status(status);
        }
        return fast_collection_status;
    }

    [[nodiscard]] MetricStatus read_disk(
        IoEntityCounters* destination,
        const std::size_t capacity,
        std::size_t& count) noexcept {
        count = 0U;
        if (disk_read_counter == nullptr || disk_write_counter == nullptr ||
            fast_collection_status != MetricStatus::available) {
            return MetricStatus::temporarily_unavailable;
        }

        DWORD read_count = 0U;
        DWORD write_count = 0U;
        const auto read_status = read_array(
            disk_read_counter, read_buffer, read_count);
        if (read_status != MetricStatus::available) {
            return read_status;
        }
        const auto write_status = read_array(
            disk_write_counter, write_buffer, write_count);
        if (write_status != MetricStatus::available) {
            return write_status;
        }

        const auto* reads = reinterpret_cast<const PDH_RAW_COUNTER_ITEM_W*>(
            read_buffer.data());
        const auto* writes = reinterpret_cast<const PDH_RAW_COUNTER_ITEM_W*>(
            write_buffer.data());
        for (DWORD read_index = 0U; read_index < read_count; ++read_index) {
            std::uint64_t identity{};
            if (!disk_identity(reads[read_index].szName, identity)) {
                continue;
            }
            const PDH_RAW_COUNTER_ITEM_W* matching_write = nullptr;
            for (DWORD write_index = 0U; write_index < write_count; ++write_index) {
                if (std::wcscmp(reads[read_index].szName,
                               writes[write_index].szName) == 0) {
                    matching_write = &writes[write_index];
                    break;
                }
            }
            if (matching_write == nullptr || count >= capacity ||
                !valid_pdh_counter(reads[read_index].RawValue) ||
                !valid_pdh_counter(matching_write->RawValue) ||
                reads[read_index].RawValue.FirstValue < 0 ||
                matching_write->RawValue.FirstValue < 0) {
                return MetricStatus::temporarily_unavailable;
            }
            destination[count++] = IoEntityCounters{
                identity,
                static_cast<std::uint64_t>(reads[read_index].RawValue.FirstValue),
                static_cast<std::uint64_t>(matching_write->RawValue.FirstValue)};
        }
        return count == 0U ? MetricStatus::temporarily_unavailable
                           : MetricStatus::available;
    }

    [[nodiscard]] MetricStatus read_disk_quality(
        RawDiskQuality& destination) noexcept {
        destination = {};
        if (disk_read_latency_counter == nullptr ||
            disk_write_latency_counter == nullptr ||
            disk_service_time_counter == nullptr || disk_queue_counter == nullptr ||
            fast_collection_status != MetricStatus::available) {
            return MetricStatus::temporarily_unavailable;
        }

        DWORD read_count = 0U;
        DWORD write_count = 0U;
        DWORD service_count = 0U;
        DWORD queue_count = 0U;
        if (read_formatted_array(disk_read_latency_counter, quality_read_buffer,
                                 read_count) != MetricStatus::available ||
            read_formatted_array(disk_write_latency_counter, quality_write_buffer,
                                 write_count) != MetricStatus::available ||
            read_formatted_array(disk_service_time_counter, quality_service_buffer,
                                 service_count) != MetricStatus::available ||
            read_formatted_array(disk_queue_counter, quality_queue_buffer,
                                 queue_count) != MetricStatus::available) {
            return MetricStatus::temporarily_unavailable;
        }

        const auto* reads = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(
            quality_read_buffer.data());
        const auto* writes = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(
            quality_write_buffer.data());
        const auto* services = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(
            quality_service_buffer.data());
        const auto* queues = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(
            quality_queue_buffer.data());

        double maximum_read{};
        double maximum_write{};
        double maximum_service{};
        double maximum_queue{};
        std::uint64_t worst_device{};
        std::size_t valid_devices{};
        for (DWORD index = 0U; index < service_count; ++index) {
            std::uint64_t identity{};
            if (!disk_identity(services[index].szName, identity)) continue;
            const auto* read = matching_formatted(reads, read_count,
                                                  services[index].szName);
            const auto* write = matching_formatted(writes, write_count,
                                                   services[index].szName);
            const auto* queue = matching_formatted(queues, queue_count,
                                                   services[index].szName);
            if (read == nullptr || write == nullptr || queue == nullptr ||
                !valid_formatted(read->FmtValue) ||
                !valid_formatted(write->FmtValue) ||
                !valid_formatted(services[index].FmtValue) ||
                !valid_formatted(queue->FmtValue)) {
                continue;
            }
            ++valid_devices;
            maximum_read = (std::max)(maximum_read, read->FmtValue.doubleValue);
            maximum_write = (std::max)(maximum_write, write->FmtValue.doubleValue);
            const auto service = services[index].FmtValue.doubleValue;
            const auto queue_depth = queue->FmtValue.doubleValue;
            if (valid_devices == 1U || service > maximum_service ||
                (service == maximum_service && queue_depth > maximum_queue)) {
                worst_device = identity;
            }
            maximum_service = (std::max)(maximum_service, service);
            maximum_queue = (std::max)(maximum_queue, queue_depth);
        }
        if (valid_devices == 0U) {
            return MetricStatus::temporarily_unavailable;
        }
        destination.read_latency = MetricValue<Seconds>::available(
            Seconds{maximum_read});
        destination.write_latency = MetricValue<Seconds>::available(
            Seconds{maximum_write});
        destination.service_time = MetricValue<Seconds>::available(
            Seconds{maximum_service});
        destination.queue_depth = MetricValue<double>::available(maximum_queue);
        destination.worst_device_id = MetricValue<std::uint64_t>::available(worst_device);
        return MetricStatus::available;
    }

    [[nodiscard]] MetricStatus read_network(
        IoEntityCounters* destination,
        const std::size_t capacity,
        std::size_t& count,
        RawNetworkQuality& quality,
        MetricStatus& quality_status) noexcept {
        count = 0U;
        quality = {};
        quality_status = MetricStatus::temporarily_unavailable;
        std::array<std::uint64_t, maximum_tracked_interfaces> inventory{};
        std::size_t inventory_count{};
        bool inventory_overflow{};
        {
            const std::scoped_lock lock{interface_inventory_mutex};
            inventory = interface_inventory;
            inventory_count = interface_inventory_count;
            inventory_overflow = interface_inventory_overflow;
        }
        if (inventory_overflow) {
            return MetricStatus::temporarily_unavailable;
        }

        std::array<InterfaceState, maximum_tracked_interfaces> current_interfaces{};
        std::size_t current_interface_count{};
        std::uint64_t active_interfaces{};
        for (std::size_t index = 0U; index < inventory_count; ++index) {
            MIB_IF_ROW2 row{};
            row.InterfaceLuid.Value = inventory[index];
            const auto row_status = GetIfEntry2(&row);
            if (row_status != NO_ERROR) {
                continue;
            }
            const auto flags = row.InterfaceAndOperStatusFlags;
            if (current_interface_count >= current_interfaces.size()) {
                return MetricStatus::temporarily_unavailable;
            }
            const auto state_flags = static_cast<std::uint8_t>(
                (flags.NotMediaConnected != 0U ? 1U : 0U) |
                (flags.NotAuthenticated != 0U ? 2U : 0U) |
                (flags.Paused != 0U ? 4U : 0U) |
                (flags.LowPower != 0U ? 8U : 0U));
            current_interfaces[current_interface_count++] = InterfaceState{
                row.InterfaceLuid.Value, static_cast<std::uint32_t>(row.OperStatus),
                state_flags};
            const bool selected = row.OperStatus == IfOperStatusUp &&
                                  flags.NotMediaConnected == 0U &&
                                  flags.NotAuthenticated == 0U;
            if (!selected) continue;
            ++active_interfaces;
            if (count >= capacity) {
                count = 0U;
                return MetricStatus::temporarily_unavailable;
            }
            destination[count++] = IoEntityCounters{
                row.InterfaceLuid.Value, row.InOctets, row.OutOctets};
        }

        std::sort(current_interfaces.begin(),
                  current_interfaces.begin() +
                      static_cast<std::ptrdiff_t>(current_interface_count));
        const auto connectivity = active_interfaces == 0U
            ? NetworkConnectivityLevel::disconnected
            : connectivity_hint_ready.load(std::memory_order_acquire)
                  ? connectivity_hint.load(std::memory_order_relaxed)
                  : NetworkConnectivityLevel::local;

        if (interfaces_warmed &&
            (current_interface_count != previous_interface_count ||
             !std::equal(current_interfaces.begin(),
                         current_interfaces.begin() +
                             static_cast<std::ptrdiff_t>(current_interface_count),
                         previous_interfaces.begin()) ||
             connectivity != previous_connectivity)) {
            ++interface_change_counter;
        }
        previous_interfaces = current_interfaces;
        previous_interface_count = current_interface_count;
        previous_connectivity = connectivity;
        interfaces_warmed = true;

        quality.connectivity = MetricValue<NetworkConnectivityLevel>::available(
            connectivity);
        quality.active_interfaces = MetricValue<std::uint64_t>::available(
            active_interfaces);
        quality.interface_change_counter = MetricValue<std::uint64_t>::available(
            interface_change_counter);
        // Connectivity/interface evidence remains useful when the independent
        // TCP statistics call fails. The caller inspects every field and marks
        // the overall observation partial without discarding healthy signals.
        quality_status = MetricStatus::available;

        MIB_TCPSTATS ipv4{};
        MIB_TCPSTATS ipv6{};
        const auto tcp_succeeded = GetTcpStatisticsEx(&ipv4, AF_INET) == NO_ERROR &&
                                   GetTcpStatisticsEx(&ipv6, AF_INET6) == NO_ERROR;
        if (tcp_succeeded) {
            const auto sum = [](const DWORD left, const DWORD right) {
                return static_cast<std::uint64_t>(left) +
                       static_cast<std::uint64_t>(right);
            };
            quality.tcp_out_segments = MetricValue<std::uint64_t>::available(
                sum(ipv4.dwOutSegs, ipv6.dwOutSegs));
            quality.tcp_retransmitted_segments = MetricValue<std::uint64_t>::available(
                sum(ipv4.dwRetransSegs, ipv6.dwRetransSegs));
            quality.tcp_failed_connections = MetricValue<std::uint64_t>::available(
                sum(ipv4.dwAttemptFails, ipv6.dwAttemptFails));
            quality.tcp_established_resets = MetricValue<std::uint64_t>::available(
                sum(ipv4.dwEstabResets, ipv6.dwEstabResets));
            quality_status = MetricStatus::available;
        } else {
            quality.tcp_out_segments = temporary<std::uint64_t>();
            quality.tcp_retransmitted_segments = temporary<std::uint64_t>();
            quality.tcp_failed_connections = temporary<std::uint64_t>();
            quality.tcp_established_resets = temporary<std::uint64_t>();
        }
        return count == 0U ? MetricStatus::temporarily_unavailable
                           : MetricStatus::available;
    }

    [[nodiscard]] MetricStatus read_processes(
        const bool collect_counters,
        const bool resolve_paths,
        RawTelemetrySnapshot& destination) {
        return process_collector.collect(
            collect_counters, resolve_paths, destination);
    }

    [[nodiscard]] bool gpu_available() const noexcept {
        return gpu_usage_counter != nullptr && gpu_dedicated_counter != nullptr &&
               gpu_shared_counter != nullptr;
    }
    [[nodiscard]] bool responsiveness_available() const noexcept {
        return dpc_time_counter != nullptr && interrupt_time_counter != nullptr &&
               dpc_rate_counter != nullptr;
    }

    [[nodiscard]] MetricStatus read_foreground(
        MetricValue<ProcessIdentity>& destination) noexcept {
        destination = temporary<ProcessIdentity>();
        const auto window = GetForegroundWindow();
        if (window == nullptr) {
            return MetricStatus::temporarily_unavailable;
        }
        DWORD pid{};
        if (GetWindowThreadProcessId(window, &pid) == 0U || pid == 0U) {
            return MetricStatus::temporarily_unavailable;
        }
        const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr) {
            destination = MetricValue<ProcessIdentity>::unavailable(
                GetLastError() == ERROR_ACCESS_DENIED ? MetricStatus::inaccessible
                                                      : MetricStatus::temporarily_unavailable);
            return destination.status;
        }
        FILETIME created{};
        FILETIME exited{};
        FILETIME kernel{};
        FILETIME user{};
        const auto succeeded = GetProcessTimes(process, &created, &exited, &kernel, &user);
        CloseHandle(process);
        if (succeeded == 0) {
            return MetricStatus::temporarily_unavailable;
        }
        const auto token = file_time_to_ticks(created);
        if (token == 0U) {
            return MetricStatus::temporarily_unavailable;
        }
        destination = MetricValue<ProcessIdentity>::available(
            ProcessIdentity{ProcessId{pid}, token});
        return MetricStatus::available;
    }

    [[nodiscard]] MetricStatus read_gpu(
        RawSystemCounters& destination,
        const MetricValue<ProcessIdentity>& foreground) noexcept {
        destination.gpu_usage = temporary<Ratio>();
        destination.gpu_dedicated_memory = temporary<ByteCount>();
        destination.gpu_shared_memory = temporary<ByteCount>();
        destination.foreground_gpu_usage = temporary<Ratio>();
        if (!gpu_available()) {
            destination.gpu_usage = MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
            destination.gpu_dedicated_memory =
                MetricValue<ByteCount>::unavailable(MetricStatus::unsupported);
            destination.gpu_shared_memory =
                MetricValue<ByteCount>::unavailable(MetricStatus::unsupported);
            destination.foreground_gpu_usage =
                MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
            return MetricStatus::unsupported;
        }
        if (fast_collection_status != MetricStatus::available) {
            return fast_collection_status;
        }
        DWORD usage_count{};
        DWORD dedicated_count{};
        DWORD shared_count{};
        if (read_formatted_array(gpu_usage_counter, gpu_usage_buffer, usage_count) !=
                MetricStatus::available ||
            read_large_array(gpu_dedicated_counter, gpu_dedicated_buffer,
                             dedicated_count) != MetricStatus::available ||
            read_large_array(gpu_shared_counter, gpu_shared_buffer,
                             shared_count) != MetricStatus::available) {
            return MetricStatus::temporarily_unavailable;
        }
        const auto* usage = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(
            gpu_usage_buffer.data());
        auto& groups = gpu_engine_groups;
        groups.fill({});
        std::size_t group_count{};
        double foreground_max{};
        bool has_foreground{};
        for (DWORD index = 0U; index < usage_count; ++index) {
            if (!valid_formatted(usage[index].FmtValue) ||
                usage[index].szName == nullptr) {
                continue;
            }
            unsigned int pid{};
            unsigned long long high{};
            unsigned long long low{};
            unsigned int physical{};
            unsigned int engine{};
            if (swscanf_s(usage[index].szName,
                    L"pid_%u_luid_0x%llx_0x%llx_phys_%u_eng_%u_",
                    &pid, &high, &low, &physical, &engine) != 5) {
                continue;
            }
            auto group = groups.begin();
            for (; group != groups.begin() + static_cast<std::ptrdiff_t>(group_count);
                 ++group) {
                if (group->luid_high == high && group->luid_low == low &&
                    group->physical == physical && group->engine == engine) {
                    break;
                }
            }
            if (group == groups.begin() + static_cast<std::ptrdiff_t>(group_count)) {
                if (group_count >= groups.size()) {
                    return MetricStatus::temporarily_unavailable;
                }
                *group = EngineGroup{high, low, physical, engine, 0.0};
                ++group_count;
            }
            group->usage += usage[index].FmtValue.doubleValue;
            if (foreground.has_value() && pid == foreground.value.pid.value) {
                foreground_max = (std::max)(foreground_max,
                    usage[index].FmtValue.doubleValue);
                has_foreground = true;
            }
        }
        if (group_count == 0U) {
            return MetricStatus::temporarily_unavailable;
        }
        double busiest_engine{};
        for (std::size_t index = 0U; index < group_count; ++index) {
            busiest_engine = (std::max)(busiest_engine, groups[index].usage);
        }
        destination.gpu_usage = MetricValue<Ratio>::available(
            Ratio{(std::clamp)(busiest_engine / 100.0, 0.0, 1.0)});
        destination.foreground_gpu_usage = has_foreground
            ? MetricValue<Ratio>::available(
                  Ratio{(std::clamp)(foreground_max / 100.0, 0.0, 1.0)})
            : MetricValue<Ratio>::unavailable(
                  foreground.has_value() ? MetricStatus::temporarily_unavailable
                                         : foreground.status);

        const auto sum_memory = [](const PDH_FMT_COUNTERVALUE_ITEM_W* values,
                                   const DWORD count,
                                   MetricValue<ByteCount>& output) {
            std::uint64_t total{};
            std::size_t valid{};
            for (DWORD index = 0U; index < count; ++index) {
                const auto& value = values[index].FmtValue;
                if ((value.CStatus != PDH_CSTATUS_VALID_DATA &&
                     value.CStatus != PDH_CSTATUS_NEW_DATA) ||
                    value.largeValue < 0) {
                    continue;
                }
                const auto bytes = static_cast<std::uint64_t>(value.largeValue);
                if (UINT64_MAX - total < bytes) {
                    output = temporary<ByteCount>();
                    return;
                }
                total += bytes;
                ++valid;
            }
            output = valid != 0U
                         ? MetricValue<ByteCount>::available(ByteCount{total})
                         : temporary<ByteCount>();
        };
        sum_memory(reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(
                       gpu_dedicated_buffer.data()), dedicated_count,
                   destination.gpu_dedicated_memory);
        sum_memory(reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(
                       gpu_shared_buffer.data()), shared_count,
                   destination.gpu_shared_memory);
        return destination.gpu_dedicated_memory.has_value() &&
                       destination.gpu_shared_memory.has_value()
                   ? MetricStatus::available
                   : MetricStatus::temporarily_unavailable;
    }

    [[nodiscard]] MetricStatus read_responsiveness(
        RawSystemCounters& destination) noexcept {
        destination.dpc_usage = temporary<Ratio>();
        destination.interrupt_usage = temporary<Ratio>();
        destination.dpc_rate = temporary<double>();
        if (!responsiveness_available()) {
            destination.dpc_usage = MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
            destination.interrupt_usage =
                MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
            destination.dpc_rate = MetricValue<double>::unavailable(MetricStatus::unsupported);
            return MetricStatus::unsupported;
        }
        if (fast_collection_status != MetricStatus::available) {
            return fast_collection_status;
        }
        const auto read_value = [](const PDH_HCOUNTER counter,
                                   double& value) noexcept {
            PDH_FMT_COUNTERVALUE formatted{};
            const auto status = PdhGetFormattedCounterValue(
                counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &formatted);
            if (status != ERROR_SUCCESS ||
                (formatted.CStatus != PDH_CSTATUS_VALID_DATA &&
                 formatted.CStatus != PDH_CSTATUS_NEW_DATA) ||
                !std::isfinite(formatted.doubleValue) || formatted.doubleValue < 0.0) {
                return false;
            }
            value = formatted.doubleValue;
            return true;
        };
        double dpc{};
        double interrupt{};
        double rate{};
        if (!read_value(dpc_time_counter, dpc) ||
            !read_value(interrupt_time_counter, interrupt) ||
            !read_value(dpc_rate_counter, rate)) {
            return MetricStatus::temporarily_unavailable;
        }
        destination.dpc_usage = MetricValue<Ratio>::available(
            Ratio{(std::clamp)(dpc / 100.0, 0.0, 1.0)});
        destination.interrupt_usage = MetricValue<Ratio>::available(
            Ratio{(std::clamp)(interrupt / 100.0, 0.0, 1.0)});
        destination.dpc_rate = MetricValue<double>::available(rate);
        return MetricStatus::available;
    }

    [[nodiscard]] MetricStatus read_power(RawSystemCounters& destination) noexcept {
        destination.cpu_current_mhz = temporary<double>();
        destination.cpu_max_mhz = temporary<double>();
        destination.cpu_thermal_limit_mhz = temporary<double>();
        destination.cpu_thermal_limit_fraction = temporary<Ratio>();
        destination.power_source = temporary<PowerSource>();
        destination.battery_fraction = temporary<Ratio>();
        destination.battery_saver = temporary<bool>();
        destination.system_uptime = MetricValue<Seconds>::available(
            Seconds{static_cast<double>(GetTickCount64()) / 1'000.0});

        const auto processor_count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (processor_count == 0U || processor_count > processor_power.size()) {
            return MetricStatus::temporarily_unavailable;
        }
        const auto status = CallNtPowerInformation(
            ProcessorInformation, nullptr, 0U, processor_power.data(),
            static_cast<ULONG>(processor_count * sizeof(ProcessorPowerInformation)));
        if (status == 0) {
            double current{};
            double maximum{};
            double limit{};
            std::size_t valid{};
            for (DWORD index = 0U; index < processor_count; ++index) {
                const auto& value = processor_power[index];
                if (value.max_mhz == 0U || value.current_mhz == 0U ||
                    value.mhz_limit == 0U) {
                    continue;
                }
                current += value.current_mhz;
                maximum += value.max_mhz;
                limit += value.mhz_limit;
                ++valid;
            }
            if (valid != 0U) {
                const auto divisor = static_cast<double>(valid);
                destination.cpu_current_mhz = MetricValue<double>::available(current / divisor);
                destination.cpu_max_mhz = MetricValue<double>::available(maximum / divisor);
                destination.cpu_thermal_limit_mhz =
                    MetricValue<double>::available(limit / divisor);
                destination.cpu_thermal_limit_fraction = MetricValue<Ratio>::available(
                    Ratio{(std::clamp)(limit / maximum, 0.0, 1.0)});
            }
        }

        SYSTEM_POWER_STATUS power{};
        if (GetSystemPowerStatus(&power) != 0) {
            auto source = PowerSource::unknown;
            if (power.ACLineStatus == 1U) source = PowerSource::ac;
            else if (power.ACLineStatus == 0U) source = PowerSource::battery;
            destination.power_source = MetricValue<PowerSource>::available(source);
            destination.battery_saver = MetricValue<bool>::available(
                power.SystemStatusFlag != 0U);
            destination.battery_fraction = power.BatteryLifePercent <= 100U
                ? MetricValue<Ratio>::available(
                      Ratio{static_cast<double>(power.BatteryLifePercent) / 100.0})
                : MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
        }
        return destination.cpu_current_mhz.has_value() ||
                       destination.power_source.has_value()
                   ? MetricStatus::available
                   : MetricStatus::temporarily_unavailable;
    }

    [[nodiscard]] GpuInventoryEvidence inventory() const noexcept {
        return gpu_inventory_evidence;
    }

private:
    static constexpr std::size_t pdh_buffer_size = 64U * 1024U;
    using PdhBuffer = std::array<std::byte, pdh_buffer_size>;

    struct InterfaceState {
        std::uint64_t identity{};
        std::uint32_t operational_status{};
        std::uint8_t state_flags{};
        friend constexpr auto operator<=>(const InterfaceState&,
                                          const InterfaceState&) = default;
    };

    struct EngineGroup {
        unsigned long long luid_high{};
        unsigned long long luid_low{};
        unsigned int physical{};
        unsigned int engine{};
        double usage{};
    };

    static constexpr std::size_t maximum_tracked_interfaces = 128U;

    [[nodiscard]] static bool valid_formatted(
        const PDH_FMT_COUNTERVALUE& value) noexcept {
        return (value.CStatus == PDH_CSTATUS_VALID_DATA ||
                value.CStatus == PDH_CSTATUS_NEW_DATA) &&
               std::isfinite(value.doubleValue) && value.doubleValue >= 0.0;
    }

    [[nodiscard]] static const PDH_FMT_COUNTERVALUE_ITEM_W* matching_formatted(
        const PDH_FMT_COUNTERVALUE_ITEM_W* values,
        const DWORD count,
        const wchar_t* name) noexcept {
        for (DWORD index = 0U; index < count; ++index) {
            if (std::wcscmp(values[index].szName, name) == 0) return &values[index];
        }
        return nullptr;
    }

    [[nodiscard]] static MetricStatus read_array(
        const PDH_HCOUNTER counter,
        PdhBuffer& buffer,
        DWORD& count) noexcept {
        DWORD bytes = 0U;
        count = 0U;
        auto status = PdhGetRawCounterArrayW(counter, &bytes, &count, nullptr);
        if (status != PDH_MORE_DATA) {
            return pdh_error_status(status);
        }
        if (bytes == 0U || bytes > buffer.size()) {
            return MetricStatus::temporarily_unavailable;
        }
        status = PdhGetRawCounterArrayW(
            counter, &bytes, &count,
            reinterpret_cast<PDH_RAW_COUNTER_ITEM_W*>(buffer.data()));
        return status == ERROR_SUCCESS ? MetricStatus::available
                                       : pdh_error_status(status);
    }

    [[nodiscard]] static MetricStatus read_formatted_array(
        const PDH_HCOUNTER counter,
        PdhBuffer& buffer,
        DWORD& count) noexcept {
        DWORD bytes = 0U;
        count = 0U;
        auto status = PdhGetFormattedCounterArrayW(
            counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
            &bytes, &count, nullptr);
        if (status != PDH_MORE_DATA || bytes == 0U || bytes > buffer.size()) {
            return pdh_error_status(status);
        }
        status = PdhGetFormattedCounterArrayW(
            counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
            &bytes, &count,
            reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data()));
        return status == ERROR_SUCCESS ? MetricStatus::available
                                       : pdh_error_status(status);
    }

    [[nodiscard]] static MetricStatus read_large_array(
        const PDH_HCOUNTER counter,
        PdhBuffer& buffer,
        DWORD& count) noexcept {
        DWORD bytes{};
        count = 0U;
        auto status = PdhGetFormattedCounterArrayW(
            counter, PDH_FMT_LARGE, &bytes, &count, nullptr);
        if (status != PDH_MORE_DATA || bytes == 0U || bytes > buffer.size()) {
            return pdh_error_status(status);
        }
        status = PdhGetFormattedCounterArrayW(
            counter, PDH_FMT_LARGE, &bytes, &count,
            reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data()));
        return status == ERROR_SUCCESS ? MetricStatus::available
                                       : pdh_error_status(status);
    }

    PDH_HQUERY fast_query{};
    MetricStatus fast_collection_status{MetricStatus::temporarily_unavailable};
    PDH_HCOUNTER disk_read_counter{};
    PDH_HCOUNTER disk_write_counter{};
    alignas(PDH_RAW_COUNTER_ITEM_W) PdhBuffer read_buffer{};
    alignas(PDH_RAW_COUNTER_ITEM_W) PdhBuffer write_buffer{};
    PDH_HCOUNTER disk_read_latency_counter{};
    PDH_HCOUNTER disk_write_latency_counter{};
    PDH_HCOUNTER disk_service_time_counter{};
    PDH_HCOUNTER disk_queue_counter{};
    alignas(PDH_FMT_COUNTERVALUE_ITEM_W) PdhBuffer quality_read_buffer{};
    alignas(PDH_FMT_COUNTERVALUE_ITEM_W) PdhBuffer quality_write_buffer{};
    alignas(PDH_FMT_COUNTERVALUE_ITEM_W) PdhBuffer quality_service_buffer{};
    alignas(PDH_FMT_COUNTERVALUE_ITEM_W) PdhBuffer quality_queue_buffer{};
    PDH_HCOUNTER gpu_usage_counter{};
    PDH_HCOUNTER gpu_dedicated_counter{};
    PDH_HCOUNTER gpu_shared_counter{};
    alignas(PDH_FMT_COUNTERVALUE_ITEM_W) PdhBuffer gpu_usage_buffer{};
    alignas(PDH_FMT_COUNTERVALUE_ITEM_W) PdhBuffer gpu_dedicated_buffer{};
    alignas(PDH_FMT_COUNTERVALUE_ITEM_W) PdhBuffer gpu_shared_buffer{};
    // GPU instance grouping is bounded but large enough to exceed the MSVC
    // analyzer's safe stack-frame threshold. NativeState already lives on the
    // heap and owns the other reusable PDH scratch buffers.
    std::array<EngineGroup, 512U> gpu_engine_groups{};
    PDH_HCOUNTER dpc_time_counter{};
    PDH_HCOUNTER interrupt_time_counter{};
    PDH_HCOUNTER dpc_rate_counter{};
    std::array<ProcessorPowerInformation, 1'024U> processor_power{};
    std::array<InterfaceState, maximum_tracked_interfaces> previous_interfaces{};
    std::size_t previous_interface_count{};
    NetworkConnectivityLevel previous_connectivity{NetworkConnectivityLevel::unknown};
    std::uint64_t interface_change_counter{};
    bool interfaces_warmed{};
    HANDLE interface_notification{};
    HANDLE connectivity_notification{};
    std::atomic<NetworkConnectivityLevel> connectivity_hint{
        NetworkConnectivityLevel::unknown};
    std::atomic_bool connectivity_hint_ready{};
    std::mutex interface_inventory_mutex{};
    std::array<std::uint64_t, maximum_tracked_interfaces> interface_inventory{};
    std::size_t interface_inventory_count{};
    bool interface_inventory_overflow{};
    WindowsProcessCollector process_collector{};
    GpuInventoryEvidence gpu_inventory_evidence{};
};

WindowsTelemetryFunctions default_windows_telemetry_functions() noexcept {
    auto functions = WindowsTelemetryFunctions{read_system_times, read_physical_memory};
    functions.prepare_sampling_thread = prepare_sampling_thread;
    return functions;
}

WindowsTelemetryProvider::WindowsTelemetryProvider(
    const core::IMonotonicClock& clock) noexcept
    : clock_{clock},
      functions_{default_windows_telemetry_functions()},
      native_state_{new (std::nothrow) NativeState{}} {}

WindowsTelemetryProvider::WindowsTelemetryProvider(
    const core::IMonotonicClock& clock,
    const WindowsTelemetryFunctions functions) noexcept
    : clock_{clock}, functions_{functions} {}

WindowsTelemetryProvider::~WindowsTelemetryProvider() = default;

bool WindowsTelemetryProvider::prepare_sampling_thread() noexcept {
    return functions_.prepare_sampling_thread != nullptr &&
           functions_.prepare_sampling_thread();
}

ProviderSampleResult WindowsTelemetryProvider::sample(
    const SamplingRequest request,
    RawTelemetrySnapshot& destination) {
    destination.reset(clock_.now(), request.tiers);

    destination.system.cpu_time = temporary<CpuTimeCounters>();
    destination.system.memory_total = temporary<ByteCount>();
    destination.system.memory_available = temporary<ByteCount>();
    destination.system.disk_read_bytes = temporary<ByteCount>();
    destination.system.disk_write_bytes = temporary<ByteCount>();
    destination.system.network_receive_bytes = temporary<ByteCount>();
    destination.system.network_transmit_bytes = temporary<ByteCount>();
    destination.system.disk_quality.read_latency = temporary<Seconds>();
    destination.system.disk_quality.write_latency = temporary<Seconds>();
    destination.system.disk_quality.service_time = temporary<Seconds>();
    destination.system.disk_quality.queue_depth = temporary<double>();
    destination.system.disk_quality.worst_device_id = temporary<std::uint64_t>();
    destination.system.network_quality.connectivity =
        temporary<NetworkConnectivityLevel>();
    destination.system.network_quality.active_interfaces = temporary<std::uint64_t>();
    destination.system.network_quality.interface_change_counter =
        temporary<std::uint64_t>();
    destination.system.network_quality.tcp_out_segments = temporary<std::uint64_t>();
    destination.system.network_quality.tcp_retransmitted_segments =
        temporary<std::uint64_t>();
    destination.system.network_quality.tcp_failed_connections =
        temporary<std::uint64_t>();
    destination.system.network_quality.tcp_established_resets =
        temporary<std::uint64_t>();
    destination.system.logical_processor_count = temporary<std::uint32_t>();
    destination.system.gpu_usage = temporary<Ratio>();
    destination.system.gpu_dedicated_memory = temporary<ByteCount>();
    destination.system.gpu_shared_memory = temporary<ByteCount>();
    destination.system.foreground_process = request.collect_foreground_application
        ? temporary<ProcessIdentity>()
        : MetricValue<ProcessIdentity>::unavailable(MetricStatus::unsupported);
    destination.system.foreground_gpu_usage = request.collect_foreground_application
        ? temporary<Ratio>()
        : MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
    destination.system.dpc_usage = temporary<Ratio>();
    destination.system.interrupt_usage = temporary<Ratio>();
    destination.system.dpc_rate = temporary<double>();
    destination.system.cpu_current_mhz = temporary<double>();
    destination.system.cpu_max_mhz = temporary<double>();
    destination.system.cpu_thermal_limit_mhz = temporary<double>();
    destination.system.cpu_thermal_limit_fraction = temporary<Ratio>();
    destination.system.power_source = temporary<PowerSource>();
    destination.system.battery_fraction = temporary<Ratio>();
    destination.system.battery_saver = temporary<bool>();
    destination.system.system_uptime = temporary<Seconds>();

    std::uint32_t attempted = 0U;
    std::uint32_t failed = 0U;

    if (request.collect_foreground_application && native_state_ != nullptr) {
        static_cast<void>(native_state_->read_foreground(
            destination.system.foreground_process));
    }

    if (request.tiers.contains(SamplingTier::fast)) {
        if (native_state_ != nullptr) {
            static_cast<void>(native_state_->collect_fast());
        }

        ++attempted;
        WindowsSystemTimes times{};
        if (functions_.read_system_times != nullptr && functions_.read_system_times(times)) {
            destination.system.cpu_time = convert_system_times(
                times.idle_ticks,
                times.kernel_ticks_including_idle,
                times.user_ticks);
            if (!destination.system.cpu_time.has_value()) {
                ++failed;
            }
        } else {
            ++failed;
        }

        ++attempted;
        std::size_t disk_count = 0U;
        const auto disk_status = native_state_ != nullptr
                                     ? native_state_->read_disk(
                                           io_buffer_.data(), io_buffer_.size(), disk_count)
                                     : functions_.read_disk_counters != nullptr
                                           ? functions_.read_disk_counters(
                                                 functions_.io_context, io_buffer_.data(),
                                                 io_buffer_.size(), disk_count)
                                           : MetricStatus::temporarily_unavailable;
        if (disk_status == MetricStatus::available) {
            const auto aggregate = disk_tracker_.update(
                std::span<const IoEntityCounters>{io_buffer_.data(), disk_count});
            destination.system.disk_read_bytes = aggregate.first;
            destination.system.disk_write_bytes = aggregate.second;
            if (!aggregate.first.has_value() || !aggregate.second.has_value()) {
                ++failed;
            }
        } else {
            destination.system.disk_read_bytes =
                MetricValue<ByteCount>::unavailable(disk_status);
            destination.system.disk_write_bytes =
                MetricValue<ByteCount>::unavailable(disk_status);
            ++failed;
        }

        ++attempted;
        const auto disk_quality_status = native_state_ != nullptr
                                             ? native_state_->read_disk_quality(
                                                   destination.system.disk_quality)
                                             : functions_.read_disk_quality != nullptr
                                                   ? functions_.read_disk_quality(
                                                         functions_.io_context,
                                                         destination.system.disk_quality)
                                                   : MetricStatus::temporarily_unavailable;
        if (disk_quality_status != MetricStatus::available) {
            destination.system.disk_quality.read_latency =
                MetricValue<Seconds>::unavailable(disk_quality_status);
            destination.system.disk_quality.write_latency =
                MetricValue<Seconds>::unavailable(disk_quality_status);
            destination.system.disk_quality.service_time =
                MetricValue<Seconds>::unavailable(disk_quality_status);
            destination.system.disk_quality.queue_depth =
                MetricValue<double>::unavailable(disk_quality_status);
            destination.system.disk_quality.worst_device_id =
                MetricValue<std::uint64_t>::unavailable(disk_quality_status);
            ++failed;
        }

        ++attempted;
        std::size_t network_count = 0U;
        MetricStatus network_quality_status = MetricStatus::temporarily_unavailable;
        const auto network_status = native_state_ != nullptr
                                        ? native_state_->read_network(
                                              io_buffer_.data(), io_buffer_.size(), network_count,
                                              destination.system.network_quality,
                                              network_quality_status)
                                        : functions_.read_network_counters != nullptr
                                              ? functions_.read_network_counters(
                                                    functions_.io_context, io_buffer_.data(),
                                                    io_buffer_.size(), network_count)
                                              : MetricStatus::temporarily_unavailable;
        if (native_state_ == nullptr && functions_.read_network_quality != nullptr) {
            network_quality_status = functions_.read_network_quality(
                functions_.io_context, destination.system.network_quality);
        }
        if (network_status == MetricStatus::available) {
            const auto aggregate = network_tracker_.update(
                std::span<const IoEntityCounters>{io_buffer_.data(), network_count});
            destination.system.network_receive_bytes = aggregate.first;
            destination.system.network_transmit_bytes = aggregate.second;
            if (!aggregate.first.has_value() || !aggregate.second.has_value()) {
                ++failed;
            }
        } else {
            destination.system.network_receive_bytes =
                MetricValue<ByteCount>::unavailable(network_status);
            destination.system.network_transmit_bytes =
                MetricValue<ByteCount>::unavailable(network_status);
            ++failed;
        }
        ++attempted;
        if (network_quality_status != MetricStatus::available) {
            destination.system.network_quality.connectivity =
                MetricValue<NetworkConnectivityLevel>::unavailable(
                    network_quality_status);
            destination.system.network_quality.active_interfaces =
                MetricValue<std::uint64_t>::unavailable(network_quality_status);
            destination.system.network_quality.interface_change_counter =
                MetricValue<std::uint64_t>::unavailable(network_quality_status);
            destination.system.network_quality.tcp_out_segments =
                MetricValue<std::uint64_t>::unavailable(network_quality_status);
            destination.system.network_quality.tcp_retransmitted_segments =
                MetricValue<std::uint64_t>::unavailable(network_quality_status);
            destination.system.network_quality.tcp_failed_connections =
                MetricValue<std::uint64_t>::unavailable(network_quality_status);
            destination.system.network_quality.tcp_established_resets =
                MetricValue<std::uint64_t>::unavailable(network_quality_status);
            ++failed;
        } else if (!destination.system.network_quality.connectivity.has_value() ||
                   !destination.system.network_quality.active_interfaces.has_value() ||
                   !destination.system.network_quality.interface_change_counter.has_value() ||
                   !destination.system.network_quality.tcp_out_segments.has_value() ||
                   !destination.system.network_quality.tcp_retransmitted_segments.has_value() ||
                   !destination.system.network_quality.tcp_failed_connections.has_value() ||
                   !destination.system.network_quality.tcp_established_resets.has_value()) {
            ++failed;
        }

        if (native_state_ != nullptr && native_state_->gpu_available()) {
            static_cast<void>(native_state_->read_gpu(
                destination.system, destination.system.foreground_process));
            if (!request.collect_foreground_application) {
                destination.system.foreground_gpu_usage =
                    MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
            }
        } else {
            destination.system.gpu_usage =
                MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
            destination.system.gpu_dedicated_memory =
                MetricValue<ByteCount>::unavailable(MetricStatus::unsupported);
            destination.system.gpu_shared_memory =
                MetricValue<ByteCount>::unavailable(MetricStatus::unsupported);
            destination.system.foreground_gpu_usage =
                MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
        }

        if (native_state_ != nullptr && native_state_->responsiveness_available()) {
            static_cast<void>(native_state_->read_responsiveness(destination.system));
        } else {
            destination.system.dpc_usage =
                MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
            destination.system.interrupt_usage =
                MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
            destination.system.dpc_rate =
                MetricValue<double>::unavailable(MetricStatus::unsupported);
        }

    }

    if (request.tiers.contains(SamplingTier::normal)) {
        ++attempted;
        WindowsPhysicalMemory memory{};
        if (functions_.read_physical_memory != nullptr &&
            functions_.read_physical_memory(memory)) {
            destination.system.memory_total = MetricValue<ByteCount>::available(
                ByteCount{memory.total_bytes});
            destination.system.memory_available = MetricValue<ByteCount>::available(
                ByteCount{memory.available_bytes});
        } else {
            ++failed;
        }

        destination.system.logical_processor_count =
            MetricValue<std::uint32_t>::available(
                static_cast<std::uint32_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));

        if (native_state_ != nullptr) {
            ++attempted;
            const auto process_status = native_state_->read_processes(
                true, request.tiers.contains(SamplingTier::slow), destination);
            if (process_status != MetricStatus::available) {
                ++failed;
            }

            static_cast<void>(native_state_->read_power(destination.system));
        } else {
            destination.system.cpu_current_mhz =
                MetricValue<double>::unavailable(MetricStatus::unsupported);
            destination.system.cpu_max_mhz =
                MetricValue<double>::unavailable(MetricStatus::unsupported);
            destination.system.cpu_thermal_limit_mhz =
                MetricValue<double>::unavailable(MetricStatus::unsupported);
            destination.system.cpu_thermal_limit_fraction =
                MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
            destination.system.power_source =
                MetricValue<PowerSource>::unavailable(MetricStatus::unsupported);
            destination.system.battery_fraction =
                MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
            destination.system.battery_saver =
                MetricValue<bool>::unavailable(MetricStatus::unsupported);
            destination.system.system_uptime =
                MetricValue<Seconds>::unavailable(MetricStatus::unsupported);
        }
    }

    ProviderSampleStatus status = ProviderSampleStatus::complete;
    if (failed != 0U) {
        status = failed == attempted ? ProviderSampleStatus::temporarily_failed
                                     : ProviderSampleStatus::partial;
    }
    return ProviderSampleResult{status, sequence_++};
}

PlatformCapabilities WindowsTelemetryProvider::capabilities() const noexcept {
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
    result.gpu_usage = native_state_ != nullptr && native_state_->gpu_available();
    result.gpu_memory = result.gpu_usage;
    result.gpu_inventory = native_state_ != nullptr;
    result.foreground_application = native_state_ != nullptr;
    result.foreground_gpu_usage = result.gpu_usage;
    result.dpc_isr = native_state_ != nullptr && native_state_->responsiveness_available();
    result.cpu_frequency = native_state_ != nullptr;
    result.cpu_thermal_limit = native_state_ != nullptr;
    result.power_status = native_state_ != nullptr;
    result.system_uptime = native_state_ != nullptr;
    return result;
}

GpuInventoryEvidence WindowsTelemetryProvider::gpu_inventory() const noexcept {
    return native_state_ != nullptr ? native_state_->inventory()
                                    : GpuInventoryEvidence{};
}

} // namespace blackbox::telemetry::windows
