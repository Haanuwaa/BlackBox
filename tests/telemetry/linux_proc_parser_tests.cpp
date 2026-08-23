#include "telemetry/linux/linux_proc_parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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
