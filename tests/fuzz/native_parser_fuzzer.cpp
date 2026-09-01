#include "telemetry/linux/linux_proc_parser.hpp"
#include "telemetry/linux/linux_psi_parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    constexpr std::size_t maximum_input_size = 64U * 1024U;
    if (data == nullptr || size > maximum_input_size) return 0;

    const std::string_view text{reinterpret_cast<const char*>(data), size};
    std::array<blackbox::telemetry::IoEntityCounters, 64U> interfaces{};
    using namespace blackbox::telemetry::linux;

    static_cast<void>(parse_proc_stat(text));
    static_cast<void>(parse_proc_meminfo(text));
    static_cast<void>(parse_sys_block_stat(text));
    static_cast<void>(parse_proc_net_dev(text, interfaces));
    static_cast<void>(parse_proc_net_snmp(text));
    static_cast<void>(parse_proc_uptime(text));
    static_cast<void>(parse_sysfs_frequency_mhz(text));
    static_cast<void>(parse_sysfs_cpu_list_count(text));
    static_cast<void>(parse_linux_low_power_profile(text));
    static_cast<void>(parse_power_supply_uevent(text));
    static_cast<void>(parse_proc_pid_stat(text, 1U));
    static_cast<void>(parse_proc_pid_status_memory(text));
    static_cast<void>(parse_proc_pid_io(text));
    static_cast<void>(parse_linux_psi(text));
    return 0;
}
