#pragma once

#include "telemetry/io_counter_tracker.hpp"
#include "telemetry/types.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace blackbox::telemetry::linux {

enum class ProcParseError : std::uint8_t {
  missing_field,
  duplicate_field,
  invalid_number,
  overflow,
  invalid_relationship,
};

struct ProcCpuSnapshot {
  CpuTimeCounters counters{};
  std::uint32_t logical_processor_count{};
  friend constexpr bool operator==(const ProcCpuSnapshot &,
                                   const ProcCpuSnapshot &) = default;
};

struct ProcMemorySnapshot {
  ByteCount total{};
  ByteCount available{};
  friend constexpr bool operator==(const ProcMemorySnapshot &,
                                   const ProcMemorySnapshot &) = default;
};

struct ProcBlockSnapshot {
  std::uint64_t read_bytes{};
  std::uint64_t write_bytes{};
  friend constexpr bool operator==(const ProcBlockSnapshot &,
                                   const ProcBlockSnapshot &) = default;
};

struct ProcProcessStat {
  ProcessIdentity identity{};
  ProcessId parent_pid{};
  std::string name{};
  std::uint64_t cpu_ticks{};
  friend bool operator==(const ProcProcessStat &,
                         const ProcProcessStat &) = default;
};

struct ProcProcessIo {
  ByteCount read_bytes{};
  ByteCount write_bytes{};
  friend constexpr bool operator==(const ProcProcessIo &,
                                   const ProcProcessIo &) = default;
};

[[nodiscard]] std::expected<ProcCpuSnapshot, ProcParseError>
parse_proc_stat(std::string_view contents) noexcept;

[[nodiscard]] std::expected<ProcMemorySnapshot, ProcParseError>
parse_proc_meminfo(std::string_view contents) noexcept;

[[nodiscard]] std::expected<ProcBlockSnapshot, ProcParseError>
parse_sys_block_stat(std::string_view contents) noexcept;

[[nodiscard]] std::expected<std::size_t, ProcParseError>
parse_proc_net_dev(std::string_view contents,
                   std::span<IoEntityCounters> destination) noexcept;

[[nodiscard]] std::expected<ProcProcessStat, ProcParseError>
parse_proc_pid_stat(std::string_view contents,
                    std::uint32_t expected_pid) noexcept;

[[nodiscard]] std::expected<ByteCount, ProcParseError>
parse_proc_pid_status_memory(std::string_view contents) noexcept;

[[nodiscard]] std::expected<ProcProcessIo, ProcParseError>
parse_proc_pid_io(std::string_view contents) noexcept;

} // namespace blackbox::telemetry::linux
