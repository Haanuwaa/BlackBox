#include "telemetry/linux/linux_proc_parser.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>

namespace blackbox::telemetry::linux {
namespace {

[[nodiscard]] constexpr bool horizontal_space(const char value) noexcept {
  return value == ' ' || value == '\t';
}

[[nodiscard]] std::string_view next_line(std::string_view &contents) noexcept {
  const auto end = contents.find('\n');
  auto line = contents.substr(0U, end);
  if (!line.empty() && line.back() == '\r')
    line.remove_suffix(1U);
  contents = end == std::string_view::npos ? std::string_view{}
                                           : contents.substr(end + 1U);
  return line;
}

[[nodiscard]] bool checked_add(std::uint64_t &total,
                               const std::uint64_t value) noexcept {
  if (value > std::numeric_limits<std::uint64_t>::max() - total)
    return false;
  total += value;
  return true;
}

[[nodiscard]] std::expected<std::uint64_t, ProcParseError>
parse_unsigned(std::string_view token) noexcept {
  if (token.empty())
    return std::unexpected{ProcParseError::invalid_number};
  std::uint64_t value{};
  const auto parsed =
      std::from_chars(token.data(), token.data() + token.size(), value);
  if (parsed.ec == std::errc::result_out_of_range) {
    return std::unexpected{ProcParseError::overflow};
  }
  if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
    return std::unexpected{ProcParseError::invalid_number};
  }
  return value;
}

[[nodiscard]] bool processor_line(const std::string_view line) noexcept {
  if (!line.starts_with("cpu") || line.size() < 5U ||
      !std::isdigit(static_cast<unsigned char>(line[3]))) {
    return false;
  }
  std::size_t index = 4U;
  while (index < line.size() &&
         std::isdigit(static_cast<unsigned char>(line[index]))) {
    ++index;
  }
  return index < line.size() && horizontal_space(line[index]);
}

[[nodiscard]] std::expected<std::uint64_t, ProcParseError>
meminfo_kib(const std::string_view line, const std::string_view key) noexcept {
  if (!line.starts_with(key))
    return std::unexpected{ProcParseError::missing_field};
  auto tail = line.substr(key.size());
  while (!tail.empty() && horizontal_space(tail.front()))
    tail.remove_prefix(1U);
  const auto number_end = tail.find_first_of(" \t");
  const auto number = tail.substr(0U, number_end);
  tail = number_end == std::string_view::npos ? std::string_view{}
                                              : tail.substr(number_end);
  while (!tail.empty() && horizontal_space(tail.front()))
    tail.remove_prefix(1U);
  if (tail != "kB")
    return std::unexpected{ProcParseError::invalid_number};
  return parse_unsigned(number);
}

} // namespace

std::expected<ProcCpuSnapshot, ProcParseError>
parse_proc_stat(std::string_view contents) noexcept {
  if (contents.empty())
    return std::unexpected{ProcParseError::missing_field};
  const auto aggregate = next_line(contents);
  if (!aggregate.starts_with("cpu") || aggregate.size() < 4U ||
      !horizontal_space(aggregate[3])) {
    return std::unexpected{ProcParseError::missing_field};
  }

  std::array<std::uint64_t, 10U> fields{};
  std::size_t count{};
  auto remaining = aggregate.substr(4U);
  while (!remaining.empty()) {
    while (!remaining.empty() && horizontal_space(remaining.front())) {
      remaining.remove_prefix(1U);
    }
    if (remaining.empty())
      break;
    const auto end = remaining.find_first_of(" \t");
    if (count == fields.size())
      return std::unexpected{ProcParseError::invalid_number};
    const auto parsed = parse_unsigned(remaining.substr(0U, end));
    if (!parsed)
      return std::unexpected{parsed.error()};
    fields[count++] = *parsed;
    remaining = end == std::string_view::npos ? std::string_view{}
                                              : remaining.substr(end);
  }
  if (count < 4U)
    return std::unexpected{ProcParseError::missing_field};

  // Linux guest counters are already included in user/nice. Summing only
  // user through steal avoids double-counting guest and guest_nice.
  std::uint64_t total{};
  const auto total_fields = count < 8U ? count : 8U;
  for (std::size_t index = 0U; index < total_fields; ++index) {
    if (!checked_add(total, fields[index])) {
      return std::unexpected{ProcParseError::overflow};
    }
  }
  std::uint64_t idle = fields[3];
  if (count > 4U && !checked_add(idle, fields[4])) {
    return std::unexpected{ProcParseError::overflow};
  }
  if (total == 0U || idle > total) {
    return std::unexpected{ProcParseError::invalid_relationship};
  }

  std::uint64_t logical_processors{};
  while (!contents.empty()) {
    if (processor_line(next_line(contents)))
      ++logical_processors;
  }
  if (logical_processors == 0U ||
      logical_processors > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected{ProcParseError::missing_field};
  }
  return ProcCpuSnapshot{CpuTimeCounters{total - idle, total},
                         static_cast<std::uint32_t>(logical_processors)};
}

std::expected<ProcMemorySnapshot, ProcParseError>
parse_proc_meminfo(std::string_view contents) noexcept {
  std::uint64_t total_kib{};
  std::uint64_t available_kib{};
  bool have_total{};
  bool have_available{};
  while (!contents.empty()) {
    const auto line = next_line(contents);
    if (line.starts_with("MemTotal:")) {
      if (have_total)
        return std::unexpected{ProcParseError::duplicate_field};
      const auto parsed = meminfo_kib(line, "MemTotal:");
      if (!parsed)
        return std::unexpected{parsed.error()};
      total_kib = *parsed;
      have_total = true;
    } else if (line.starts_with("MemAvailable:")) {
      if (have_available)
        return std::unexpected{ProcParseError::duplicate_field};
      const auto parsed = meminfo_kib(line, "MemAvailable:");
      if (!parsed)
        return std::unexpected{parsed.error()};
      available_kib = *parsed;
      have_available = true;
    }
  }
  if (!have_total || !have_available) {
    return std::unexpected{ProcParseError::missing_field};
  }
  if (total_kib == 0U || available_kib > total_kib) {
    return std::unexpected{ProcParseError::invalid_relationship};
  }
  constexpr std::uint64_t bytes_per_kib = 1024U;
  if (total_kib > std::numeric_limits<std::uint64_t>::max() / bytes_per_kib ||
      available_kib >
          std::numeric_limits<std::uint64_t>::max() / bytes_per_kib) {
    return std::unexpected{ProcParseError::overflow};
  }
  return ProcMemorySnapshot{ByteCount{total_kib * bytes_per_kib},
                            ByteCount{available_kib * bytes_per_kib}};
}

} // namespace blackbox::telemetry::linux
