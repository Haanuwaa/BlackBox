#pragma once

#include "telemetry/types.hpp"

#include <cstdint>
#include <expected>
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

[[nodiscard]] std::expected<ProcCpuSnapshot, ProcParseError>
parse_proc_stat(std::string_view contents) noexcept;

[[nodiscard]] std::expected<ProcMemorySnapshot, ProcParseError>
parse_proc_meminfo(std::string_view contents) noexcept;

} // namespace blackbox::telemetry::linux
