#include "telemetry/linux/linux_proc_parser.hpp"

#include <catch2/catch_test_macros.hpp>

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
  CHECK_FALSE(linux_telemetry::parse_sys_block_stat("1 2 3").has_value());
  CHECK_FALSE(linux_telemetry::parse_sys_block_stat(
                  "1 2 18446744073709551615 4 5 6 1\n")
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
