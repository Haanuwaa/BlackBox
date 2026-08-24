#include "telemetry/linux/linux_proc_parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
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

[[nodiscard]] bool checked_multiply(const std::uint64_t value,
                                    const std::uint64_t multiplier,
                                    std::uint64_t &result) noexcept {
  if (multiplier != 0U &&
      value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
    return false;
  }
  result = value * multiplier;
  return true;
}

[[nodiscard]] std::string_view trim_horizontal(std::string_view value) noexcept {
  while (!value.empty() && horizontal_space(value.front())) value.remove_prefix(1U);
  while (!value.empty() && horizontal_space(value.back())) value.remove_suffix(1U);
  return value;
}

[[nodiscard]] std::expected<std::size_t, ProcParseError>
split_fields(std::string_view value, std::span<std::string_view> fields) noexcept {
  std::size_t count{};
  while (true) {
    value = trim_horizontal(value);
    if (value.empty()) return count;
    if (count == fields.size()) return std::unexpected{ProcParseError::overflow};
    const auto end = value.find_first_of(" \t");
    fields[count++] = value.substr(0U, end);
    value = end == std::string_view::npos ? std::string_view{} : value.substr(end);
  }
}

[[nodiscard]] constexpr std::uint64_t stable_identity(
    const std::string_view value) noexcept {
  std::uint64_t result{14695981039346656037ULL};
  for (const auto byte : value) {
    result ^= static_cast<unsigned char>(byte);
    result *= 1099511628211ULL;
  }
  return result;
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

[[nodiscard]] std::expected<double, ProcParseError>
parse_nonnegative_decimal(const std::string_view token) noexcept {
  if (token.empty()) return std::unexpected{ProcParseError::invalid_number};
  const auto point = token.find('.');
  if (point != std::string_view::npos &&
      token.find('.', point + 1U) != std::string_view::npos) {
    return std::unexpected{ProcParseError::invalid_number};
  }
  const auto whole_token = token.substr(0U, point);
  const auto fractional_token = point == std::string_view::npos
                                    ? std::string_view{}
                                    : token.substr(point + 1U);
  if (whole_token.empty() ||
      (point != std::string_view::npos && fractional_token.empty())) {
    return std::unexpected{ProcParseError::invalid_number};
  }
  const auto whole = parse_unsigned(whole_token);
  if (!whole) return std::unexpected{whole.error()};
  if (*whole > 9'007'199'254'740'991ULL || fractional_token.size() > 9U) {
    return std::unexpected{ProcParseError::overflow};
  }
  double fraction{};
  double divisor{1.0};
  for (const auto character : fractional_token) {
    if (!std::isdigit(static_cast<unsigned char>(character))) {
      return std::unexpected{ProcParseError::invalid_number};
    }
    divisor *= 10.0;
    fraction = fraction * 10.0 + static_cast<double>(character - '0');
  }
  return static_cast<double>(*whole) + fraction / divisor;
}

[[nodiscard]] std::expected<std::pair<std::string_view, std::string_view>,
                            ProcParseError>
split_key_value(const std::string_view line) noexcept {
  const auto separator = line.find('=');
  if (separator == std::string_view::npos || separator == 0U ||
      separator + 1U == line.size()) {
    return std::unexpected{ProcParseError::missing_field};
  }
  return std::pair{line.substr(0U, separator), line.substr(separator + 1U)};
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

std::expected<ProcBlockSnapshot, ProcParseError>
parse_sys_block_stat(const std::string_view contents) noexcept {
  std::array<std::string_view, 20U> fields{};
  const auto count = split_fields(contents, fields);
  if (!count) return std::unexpected{count.error()};
  if (*count < 7U) return std::unexpected{ProcParseError::missing_field};
  const auto read_sectors = parse_unsigned(fields[2]);
  const auto write_sectors = parse_unsigned(fields[6]);
  if (!read_sectors) return std::unexpected{read_sectors.error()};
  if (!write_sectors) return std::unexpected{write_sectors.error()};
  constexpr std::uint64_t kernel_sector_bytes = 512U;
  std::uint64_t read_bytes{};
  std::uint64_t write_bytes{};
  if (!checked_multiply(*read_sectors, kernel_sector_bytes, read_bytes) ||
      !checked_multiply(*write_sectors, kernel_sector_bytes, write_bytes)) {
    return std::unexpected{ProcParseError::overflow};
  }
  return ProcBlockSnapshot{read_bytes, write_bytes};
}

std::expected<std::size_t, ProcParseError>
parse_proc_net_dev(std::string_view contents,
                   const std::span<IoEntityCounters> destination) noexcept {
  if (contents.empty()) return std::unexpected{ProcParseError::missing_field};
  static_cast<void>(next_line(contents));
  static_cast<void>(next_line(contents));
  std::size_t count{};
  while (!contents.empty()) {
    const auto line = next_line(contents);
    if (trim_horizontal(line).empty()) continue;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
      return std::unexpected{ProcParseError::missing_field};
    }
    const auto name = trim_horizontal(line.substr(0U, colon));
    if (name.empty() || name.find_first_of(" \t\r\n") != std::string_view::npos) {
      return std::unexpected{ProcParseError::invalid_number};
    }
    std::array<std::string_view, 16U> fields{};
    const auto field_count = split_fields(line.substr(colon + 1U), fields);
    if (!field_count) return std::unexpected{field_count.error()};
    if (*field_count != fields.size()) {
      return std::unexpected{ProcParseError::missing_field};
    }
    if (name == "lo") continue;
    if (count == destination.size()) return std::unexpected{ProcParseError::overflow};
    const auto received = parse_unsigned(fields[0]);
    const auto transmitted = parse_unsigned(fields[8]);
    if (!received) return std::unexpected{received.error()};
    if (!transmitted) return std::unexpected{transmitted.error()};
    const auto identity = stable_identity(name);
    for (std::size_t prior = 0U; prior < count; ++prior) {
      if (destination[prior].identity == identity) {
        return std::unexpected{ProcParseError::duplicate_field};
      }
    }
    destination[count++] = IoEntityCounters{identity, *received, *transmitted};
  }
  if (count == 0U) return std::unexpected{ProcParseError::missing_field};
  return count;
}

std::expected<ProcTcpSnapshot, ProcParseError>
parse_proc_net_snmp(std::string_view contents) noexcept {
  std::optional<std::array<std::string_view, 64U>> tcp_names{};
  std::size_t tcp_name_count{};
  std::optional<std::array<std::string_view, 64U>> tcp_values{};
  std::size_t tcp_value_count{};
  while (!contents.empty()) {
    const auto line = next_line(contents);
    if (!line.starts_with("Tcp:")) continue;
    if (!tcp_names) {
      tcp_names.emplace();
      const auto count = split_fields(line.substr(4U), *tcp_names);
      if (!count) return std::unexpected{count.error()};
      tcp_name_count = *count;
    } else if (!tcp_values) {
      tcp_values.emplace();
      const auto count = split_fields(line.substr(4U), *tcp_values);
      if (!count) return std::unexpected{count.error()};
      tcp_value_count = *count;
    } else {
      return std::unexpected{ProcParseError::duplicate_field};
    }
  }
  if (!tcp_names || !tcp_values || tcp_name_count == 0U ||
      tcp_name_count != tcp_value_count) {
    return std::unexpected{ProcParseError::missing_field};
  }

  ProcTcpSnapshot result{};
  std::array<bool, 4U> found{};
  constexpr std::array<std::string_view, 4U> required{
      "OutSegs", "RetransSegs", "AttemptFails", "EstabResets"};
  std::array<std::uint64_t*, 4U> destinations{
      &result.out_segments, &result.retransmitted_segments,
      &result.failed_connections, &result.established_resets};
  for (std::size_t field = 0U; field < tcp_name_count; ++field) {
    for (std::size_t required_index = 0U;
         required_index < required.size(); ++required_index) {
      if ((*tcp_names)[field] != required[required_index]) continue;
      if (found[required_index]) {
        return std::unexpected{ProcParseError::duplicate_field};
      }
      const auto value = parse_unsigned((*tcp_values)[field]);
      if (!value) return std::unexpected{value.error()};
      *destinations[required_index] = *value;
      found[required_index] = true;
    }
  }
  if (std::find(found.begin(), found.end(), false) != found.end()) {
    return std::unexpected{ProcParseError::missing_field};
  }
  return result;
}

std::expected<Seconds, ProcParseError>
parse_proc_uptime(std::string_view contents) noexcept {
  const auto line = next_line(contents);
  while (!contents.empty()) {
    if (!trim_horizontal(next_line(contents)).empty()) {
      return std::unexpected{ProcParseError::invalid_number};
    }
  }
  std::array<std::string_view, 3U> fields{};
  const auto count = split_fields(line, fields);
  if (!count) return std::unexpected{count.error()};
  if (*count != 2U) return std::unexpected{ProcParseError::missing_field};
  const auto uptime = parse_nonnegative_decimal(fields[0]);
  const auto idle = parse_nonnegative_decimal(fields[1]);
  if (!uptime) return std::unexpected{uptime.error()};
  if (!idle) return std::unexpected{idle.error()};
  return Seconds{*uptime};
}

std::expected<ProcPowerSupplySnapshot, ProcParseError>
parse_power_supply_uevent(std::string_view contents) noexcept {
  ProcPowerSupplySnapshot result{};
  bool have_type{};
  bool have_present{};
  bool have_online{};
  bool have_capacity{};
  while (!contents.empty()) {
    const auto line = next_line(contents);
    if (line.empty()) continue;
    const auto entry = split_key_value(line);
    if (!entry) return std::unexpected{entry.error()};
    const auto &[key, value] = *entry;
    if (key == "POWER_SUPPLY_TYPE") {
      if (have_type) return std::unexpected{ProcParseError::duplicate_field};
      have_type = true;
      if (value == "Battery") result.kind = ProcPowerSupplyKind::battery;
      else if (value == "UPS") result.kind = ProcPowerSupplyKind::ups;
      else if (value == "Mains" || value == "USB" || value == "USB_C" ||
               value == "USB_PD" || value == "Wireless") {
        result.kind = ProcPowerSupplyKind::mains;
      } else {
        result.kind = ProcPowerSupplyKind::other;
      }
    } else if (key == "POWER_SUPPLY_PRESENT") {
      if (have_present) return std::unexpected{ProcParseError::duplicate_field};
      const auto parsed = parse_unsigned(value);
      if (!parsed || *parsed > 1U) {
        return std::unexpected{parsed ? ProcParseError::invalid_relationship
                                     : parsed.error()};
      }
      result.present = *parsed == 1U;
      have_present = true;
    } else if (key == "POWER_SUPPLY_ONLINE") {
      if (have_online) return std::unexpected{ProcParseError::duplicate_field};
      const auto parsed = parse_unsigned(value);
      if (!parsed || *parsed > 1U) {
        return std::unexpected{parsed ? ProcParseError::invalid_relationship
                                     : parsed.error()};
      }
      result.online = *parsed == 1U;
      have_online = true;
    } else if (key == "POWER_SUPPLY_CAPACITY") {
      if (have_capacity) return std::unexpected{ProcParseError::duplicate_field};
      const auto parsed = parse_unsigned(value);
      if (!parsed || *parsed > 100U) {
        return std::unexpected{parsed ? ProcParseError::invalid_relationship
                                     : parsed.error()};
      }
      result.capacity_fraction = Ratio{static_cast<double>(*parsed) / 100.0};
      have_capacity = true;
    }
  }
  if (!have_type) return std::unexpected{ProcParseError::missing_field};
  return result;
}

std::expected<ProcProcessStat, ProcParseError>
parse_proc_pid_stat(const std::string_view contents,
                    const std::uint32_t expected_pid) noexcept {
  const auto open = contents.find(" (");
  const auto close = contents.rfind(") ");
  if (open == std::string_view::npos || close == std::string_view::npos ||
      close <= open + 2U) {
    return std::unexpected{ProcParseError::missing_field};
  }
  const auto pid = parse_unsigned(contents.substr(0U, open));
  if (!pid) return std::unexpected{pid.error()};
  if (*pid != expected_pid || *pid > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected{ProcParseError::invalid_relationship};
  }
  const auto name = contents.substr(open + 2U, close - open - 2U);
  if (name.empty() || name.size() > 256U ||
      name.find_first_of("\r\n\0", 0U, 3U) != std::string_view::npos) {
    return std::unexpected{ProcParseError::invalid_number};
  }
  std::array<std::string_view, 64U> fields{};
  const auto field_count = split_fields(contents.substr(close + 2U), fields);
  if (!field_count) return std::unexpected{field_count.error()};
  // fields[0] is state (field 3); ppid/utime/stime/starttime are 4/14/15/22.
  if (*field_count < 20U || fields[0].size() != 1U) {
    return std::unexpected{ProcParseError::missing_field};
  }
  const auto parent = parse_unsigned(fields[1]);
  const auto user = parse_unsigned(fields[11]);
  const auto system = parse_unsigned(fields[12]);
  const auto started = parse_unsigned(fields[19]);
  if (!parent) return std::unexpected{parent.error()};
  if (!user) return std::unexpected{user.error()};
  if (!system) return std::unexpected{system.error()};
  if (!started) return std::unexpected{started.error()};
  if (*parent > std::numeric_limits<std::uint32_t>::max() || *started == 0U) {
    return std::unexpected{ProcParseError::invalid_relationship};
  }
  auto cpu_ticks = *user;
  if (!checked_add(cpu_ticks, *system)) {
    return std::unexpected{ProcParseError::overflow};
  }
  return ProcProcessStat{ProcessIdentity{ProcessId{expected_pid}, *started},
                         ProcessId{static_cast<std::uint32_t>(*parent)},
                         std::string{name}, cpu_ticks};
}

std::expected<ByteCount, ProcParseError>
parse_proc_pid_status_memory(std::string_view contents) noexcept {
  bool found{};
  std::uint64_t kib{};
  while (!contents.empty()) {
    const auto line = next_line(contents);
    if (!line.starts_with("VmRSS:")) continue;
    if (found) return std::unexpected{ProcParseError::duplicate_field};
    const auto parsed = meminfo_kib(line, "VmRSS:");
    if (!parsed) return std::unexpected{parsed.error()};
    kib = *parsed;
    found = true;
  }
  if (!found) return std::unexpected{ProcParseError::missing_field};
  std::uint64_t bytes{};
  if (!checked_multiply(kib, 1024U, bytes)) {
    return std::unexpected{ProcParseError::overflow};
  }
  return ByteCount{bytes};
}

std::expected<ProcProcessIo, ProcParseError>
parse_proc_pid_io(std::string_view contents) noexcept {
  std::uint64_t read{};
  std::uint64_t write{};
  bool have_read{};
  bool have_write{};
  while (!contents.empty()) {
    const auto line = next_line(contents);
    const auto parse_line = [&](const std::string_view key,
                                std::uint64_t &value,
                                bool &found) -> std::expected<void, ProcParseError> {
      if (!line.starts_with(key)) return {};
      if (found) return std::unexpected{ProcParseError::duplicate_field};
      const auto parsed = parse_unsigned(trim_horizontal(line.substr(key.size())));
      if (!parsed) return std::unexpected{parsed.error()};
      value = *parsed;
      found = true;
      return {};
    };
    const auto read_result = parse_line("read_bytes:", read, have_read);
    if (!read_result) return std::unexpected{read_result.error()};
    const auto write_result = parse_line("write_bytes:", write, have_write);
    if (!write_result) return std::unexpected{write_result.error()};
  }
  if (!have_read || !have_write) {
    return std::unexpected{ProcParseError::missing_field};
  }
  return ProcProcessIo{ByteCount{read}, ByteCount{write}};
}

} // namespace blackbox::telemetry::linux
