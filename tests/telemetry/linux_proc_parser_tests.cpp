#include "telemetry/linux/linux_proc_parser.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdint>
#include <array>
#include <string>

namespace linux_telemetry = blackbox::telemetry::linux;

TEST_CASE("Linux proc stat parsing preserves cumulative CPU semantics",
          "[telemetry][linux]") {
  const auto parsed =
      linux_telemetry::parse_proc_stat("cpu  100 20 30 400 50 6 7 8 9 10\n"
                                       "cpu0 1 2 3 4 5 6 7 8 9 10\n"
                                       "cpu1 1 2 3 4 5 6 7 8 9 10\n"
                                       "intr 123\n");

  REQUIRE(parsed.has_value());
  CHECK(parsed->counters.total_ticks == 621U);
  CHECK(parsed->counters.busy_ticks == 171U);
  CHECK(parsed->logical_processor_count == 2U);
}

TEST_CASE("Linux proc memory parsing converts kernel KiB to bytes",
          "[telemetry][linux]") {
  const auto parsed =
      linux_telemetry::parse_proc_meminfo("MemTotal:       16384 kB\n"
                                          "MemFree:         1024 kB\n"
                                          "MemAvailable:    4096 kB\n");

  REQUIRE(parsed.has_value());
  CHECK(parsed->total.value == 16U * 1024U * 1024U);
  CHECK(parsed->available.value == 4U * 1024U * 1024U);
}

TEST_CASE("Linux proc parsers reject incomplete malformed and impossible data",
          "[telemetry][linux]") {
  CHECK_FALSE(linux_telemetry::parse_proc_stat("cpu 1 2 3\n").has_value());
  CHECK_FALSE(linux_telemetry::parse_proc_stat("cpu 1 2 nope 4\ncpu0 1 2 3 4\n")
                  .has_value());
  CHECK_FALSE(linux_telemetry::parse_proc_meminfo(
                  "MemTotal: 10 kB\nMemAvailable: 11 kB\n")
                  .has_value());
  CHECK_FALSE(linux_telemetry::parse_proc_meminfo(
                  "MemTotal: 10 MB\nMemAvailable: 5 kB\n")
                  .has_value());
  CHECK_FALSE(linux_telemetry::parse_proc_meminfo(
                  "MemTotal: 10 kB\nMemTotal: 10 kB\nMemAvailable: 5 kB\n")
                  .has_value());
}

TEST_CASE("Linux proc parsers reject cumulative and byte overflow",
          "[telemetry][linux]") {
  CHECK_FALSE(linux_telemetry::parse_proc_stat(
                  "cpu 18446744073709551615 1 0 0\ncpu0 1 0 0 1\n")
                  .has_value());
  CHECK_FALSE(
      linux_telemetry::parse_proc_meminfo("MemTotal: 18014398509481984 kB\n"
                                          "MemAvailable: 1 kB\n")
          .has_value());
}

TEST_CASE("Linux block and network parsers preserve cumulative byte semantics",
          "[telemetry][linux]") {
  const auto disk = linux_telemetry::parse_sys_block_stat(
      "11 2 100 4 22 3 200 8 0 10 12\n");
  REQUIRE(disk.has_value());
  CHECK(disk->read_bytes == 100U * 512U);
  CHECK(disk->write_bytes == 200U * 512U);
  CHECK(disk->read_operations == 11U);
  CHECK(disk->write_operations == 22U);
  CHECK(disk->read_time_nanoseconds == 4'000'000U);
  CHECK(disk->write_time_nanoseconds == 8'000'000U);
  CHECK(disk->weighted_time_nanoseconds == 12'000'000U);

  std::array<blackbox::telemetry::IoEntityCounters, 4U> interfaces{};
  const auto count = linux_telemetry::parse_proc_net_dev(
      "Inter-| Receive | Transmit\n"
      " face |bytes packets errs drop fifo frame compressed multicast|bytes packets errs drop fifo colls carrier compressed\n"
      " lo: 99 1 0 0 0 0 0 0 99 1 0 0 0 0 0 0\n"
      " eth0: 1000 1 0 0 0 0 0 0 2000 2 0 0 0 0 0 0\n",
      interfaces);
  REQUIRE(count.has_value());
  REQUIRE(*count == 1U);
  CHECK(interfaces[0].first_bytes == 1000U);
  CHECK(interfaces[0].second_bytes == 2000U);
}

TEST_CASE("Linux TCP uptime and power parsers preserve portable evidence semantics",
          "[telemetry][linux]") {
  const auto tcp = linux_telemetry::parse_proc_net_snmp(
      "Ip: Forwarding DefaultTTL\n"
      "Ip: 1 64\n"
      "Tcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens "
      "AttemptFails EstabResets CurrEstab InSegs OutSegs RetransSegs\n"
      "Tcp: 1 200 120000 -1 10 20 3 4 2 1000 900 12\n");
  REQUIRE(tcp.has_value());
  CHECK(tcp->out_segments == 900U);
  CHECK(tcp->retransmitted_segments == 12U);
  CHECK(tcp->failed_connections == 3U);
  CHECK(tcp->established_resets == 4U);

  const auto uptime = linux_telemetry::parse_proc_uptime("12345.67 8000.01\n");
  REQUIRE(uptime.has_value());
  CHECK(uptime->value == Catch::Approx{12345.67});

  const auto battery = linux_telemetry::parse_power_supply_uevent(
      "POWER_SUPPLY_NAME=BAT0\n"
      "POWER_SUPPLY_TYPE=Battery\n"
      "POWER_SUPPLY_PRESENT=1\n"
      "POWER_SUPPLY_CAPACITY=73\n");
  REQUIRE(battery.has_value());
  CHECK(battery->kind == linux_telemetry::ProcPowerSupplyKind::battery);
  CHECK(battery->present);
  REQUIRE(battery->capacity_fraction.has_value());
  CHECK(battery->capacity_fraction->value == Catch::Approx{0.73});

  const auto mains = linux_telemetry::parse_power_supply_uevent(
      "POWER_SUPPLY_TYPE=USB_C\nPOWER_SUPPLY_ONLINE=1\n");
  REQUIRE(mains.has_value());
  CHECK(mains->kind == linux_telemetry::ProcPowerSupplyKind::mains);
  REQUIRE(mains->online.has_value());
  CHECK(*mains->online);
}

TEST_CASE("Linux TCP uptime and power parsers fail closed on malformed input",
          "[telemetry][linux]") {
  CHECK_FALSE(linux_telemetry::parse_proc_net_snmp(
                  "Tcp: OutSegs RetransSegs AttemptFails\nTcp: 1 2 3\n")
                  .has_value());
  CHECK_FALSE(linux_telemetry::parse_proc_net_snmp(
                  "Tcp: OutSegs RetransSegs AttemptFails EstabResets\n"
                  "Tcp: 1 nope 3 4\n")
                  .has_value());
  CHECK_FALSE(linux_telemetry::parse_proc_uptime("-1.0 2.0\n").has_value());
  CHECK_FALSE(linux_telemetry::parse_proc_uptime("1.0\n").has_value());
  CHECK_FALSE(linux_telemetry::parse_power_supply_uevent(
                  "POWER_SUPPLY_TYPE=Battery\nPOWER_SUPPLY_CAPACITY=101\n")
                  .has_value());
  CHECK_FALSE(linux_telemetry::parse_power_supply_uevent(
                  "POWER_SUPPLY_TYPE=Battery\nPOWER_SUPPLY_TYPE=UPS\n")
                  .has_value());
}

TEST_CASE("Linux CPU frequency and platform profile parsers preserve exact units",
          "[telemetry][linux][power]") {
  const auto frequency =
      linux_telemetry::parse_sysfs_frequency_mhz("2387500\n");
  REQUIRE(frequency.has_value());
  CHECK(*frequency == Catch::Approx{2387.5});

  const auto cpus = linux_telemetry::parse_sysfs_cpu_list_count("0-3 8 10-11\n");
  REQUIRE(cpus.has_value());
  CHECK(*cpus == 7U);

  REQUIRE(linux_telemetry::parse_linux_low_power_profile("low-power\n")
              .has_value());
  CHECK(*linux_telemetry::parse_linux_low_power_profile("low-power\n"));
  REQUIRE(linux_telemetry::parse_linux_low_power_profile("balanced\n")
              .has_value());
  CHECK_FALSE(*linux_telemetry::parse_linux_low_power_profile("balanced\n"));
}

TEST_CASE("Linux CPU frequency and platform profile parsers fail closed",
          "[telemetry][linux][power]") {
  CHECK_FALSE(linux_telemetry::parse_sysfs_frequency_mhz("0\n").has_value());
  CHECK_FALSE(linux_telemetry::parse_sysfs_frequency_mhz("2000 extra\n")
                  .has_value());
  CHECK_FALSE(linux_telemetry::parse_sysfs_cpu_list_count("0-3 3-4\n")
                  .has_value());
  CHECK_FALSE(linux_telemetry::parse_sysfs_cpu_list_count("4-2\n")
                  .has_value());
  CHECK_FALSE(linux_telemetry::parse_linux_low_power_profile("vendor-turbo\n")
                  .has_value());
}

TEST_CASE("Linux process parsers retain stable identity memory and I O",
          "[telemetry][linux]") {
  const auto process = linux_telemetry::parse_proc_pid_stat(
      "42 (worker thread) S 7 2 3 4 5 6 7 8 9 10 100 20 13 14 15 16 17 18 9001 20 21\n",
      42U);
  REQUIRE(process.has_value());
  CHECK(process->identity == blackbox::telemetry::ProcessIdentity{
                                 blackbox::telemetry::ProcessId{42U}, 9001U});
  CHECK(process->parent_pid.value == 7U);
  CHECK(process->name == "worker thread");
  CHECK(process->cpu_ticks == 120U);

  const auto memory = linux_telemetry::parse_proc_pid_status_memory(
      "Name:\tworker\nVmRSS:\t2048 kB\n");
  REQUIRE(memory.has_value());
  CHECK(memory->value == 2U * 1024U * 1024U);

  const auto io = linux_telemetry::parse_proc_pid_io(
      "rchar: 1\nwchar: 2\nread_bytes: 300\nwrite_bytes: 400\n");
  REQUIRE(io.has_value());
  CHECK(io->read_bytes.value == 300U);
  CHECK(io->write_bytes.value == 400U);
}

TEST_CASE("Linux extended parsers reject truncation overflow and identity mismatch",
          "[telemetry][linux]") {
  CHECK_FALSE(linux_telemetry::parse_sys_block_stat("1 2 3 4 5 6 7").has_value());
  CHECK_FALSE(linux_telemetry::parse_sys_block_stat(
                  "1 2 18446744073709551615 4 5 6 1 8 0 10 11\n")
                  .has_value());
  CHECK_FALSE(linux_telemetry::parse_sys_block_stat(
                  "1 2 3 18446744073709551615 5 6 7 8 0 10 11\n")
                  .has_value());
  std::array<blackbox::telemetry::IoEntityCounters, 1U> interfaces{};
  CHECK_FALSE(linux_telemetry::parse_proc_net_dev(
                  "header\nheader\neth0: 1 2 3\n", interfaces)
                  .has_value());
  CHECK_FALSE(linux_telemetry::parse_proc_pid_stat(
                  "43 (worker) S 1 1 1 0 0 0 0 0 0 0 0 1 1 0 0 0 0 1 0 9\n",
                  42U)
                  .has_value());
  CHECK_FALSE(linux_telemetry::parse_proc_pid_status_memory(
                  "Name:\tworker\n")
                  .has_value());
  CHECK_FALSE(linux_telemetry::parse_proc_pid_io("read_bytes: 1\n").has_value());
}
