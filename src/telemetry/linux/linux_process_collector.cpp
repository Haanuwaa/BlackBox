#include "telemetry/linux/linux_process_collector.hpp"

#include "telemetry/linux/linux_proc_parser.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>

namespace blackbox::telemetry::linux {
namespace {

constexpr std::size_t maximum_processes = 8'192U;
constexpr std::size_t maximum_process_file_bytes = 64U * 1024U;

enum class ReadStatus { available, inaccessible, missing, failed, oversized };

[[nodiscard]] ReadStatus read_bounded(const char *path, std::string &output) {
  const auto descriptor = open(path, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    if (errno == EACCES || errno == EPERM) return ReadStatus::inaccessible;
    if (errno == ENOENT || errno == ESRCH) return ReadStatus::missing;
    return ReadStatus::failed;
  }
  output.clear();
  std::array<char, 4096U> buffer{};
  ReadStatus status = ReadStatus::available;
  while (true) {
    const auto count = read(descriptor, buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0) {
      status = errno == EACCES || errno == EPERM
                   ? ReadStatus::inaccessible
                   : ReadStatus::failed;
      break;
    }
    if (output.size() + static_cast<std::size_t>(count) >
        maximum_process_file_bytes) {
      status = ReadStatus::oversized;
      break;
    }
    output.append(buffer.data(), static_cast<std::size_t>(count));
  }
  static_cast<void>(close(descriptor));
  if (status != ReadStatus::available) output.clear();
  return output.empty() && status == ReadStatus::available ? ReadStatus::failed
                                                           : status;
}

[[nodiscard]] MetricStatus metric_status(const ReadStatus status) noexcept {
  return status == ReadStatus::inaccessible ? MetricStatus::inaccessible
                                            : MetricStatus::temporarily_unavailable;
}

[[nodiscard]] bool ticks_to_nanoseconds(const std::uint64_t ticks,
                                        const std::uint64_t ticks_per_second,
                                        std::chrono::nanoseconds &result) noexcept {
  if (ticks_per_second == 0U) return false;
  constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000U;
  const auto whole = ticks / ticks_per_second;
  const auto remainder = ticks % ticks_per_second;
  if (whole > static_cast<std::uint64_t>(
                  std::chrono::nanoseconds::max().count()) /
                  nanoseconds_per_second) {
    return false;
  }
  const auto value = whole * nanoseconds_per_second +
                     remainder * nanoseconds_per_second / ticks_per_second;
  if (value > static_cast<std::uint64_t>(
                  std::chrono::nanoseconds::max().count())) {
    return false;
  }
  result = std::chrono::nanoseconds{static_cast<std::int64_t>(value)};
  return true;
}

[[nodiscard]] bool process_path(const std::uint32_t pid, const char *leaf,
                                std::array<char, 64U> &destination) noexcept {
  const auto written = std::snprintf(destination.data(), destination.size(),
                                     "/proc/%u/%s", pid, leaf);
  return written > 0 && static_cast<std::size_t>(written) < destination.size();
}

[[nodiscard]] std::string executable_path(const std::uint32_t pid,
                                          MetricStatus &status) {
  std::array<char, 64U> path{};
  if (!process_path(pid, "exe", path)) {
    status = MetricStatus::temporarily_unavailable;
    return {};
  }
  std::array<char, 4097U> destination{};
  const auto length = readlink(path.data(), destination.data(), destination.size() - 1U);
  if (length < 0) {
    status = errno == EACCES || errno == EPERM ? MetricStatus::inaccessible
                                               : MetricStatus::temporarily_unavailable;
    return {};
  }
  if (length == 0 || static_cast<std::size_t>(length) >= destination.size()) {
    status = MetricStatus::temporarily_unavailable;
    return {};
  }
  status = MetricStatus::available;
  return std::string{destination.data(), static_cast<std::size_t>(length)};
}

} // namespace

struct LinuxProcessCollector::State {
  State() noexcept {
    const auto ticks = sysconf(_SC_CLK_TCK);
    ticks_per_second = ticks > 0 ? static_cast<std::uint64_t>(ticks) : 0U;
    contents.reserve(maximum_process_file_bytes);
  }

  [[nodiscard]] MetricStatus collect(const bool resolve_paths,
                                     RawTelemetrySnapshot &destination) {
    DIR *directory = opendir("/proc");
    if (directory == nullptr) {
      return errno == EACCES || errno == EPERM ? MetricStatus::inaccessible
                                               : MetricStatus::temporarily_unavailable;
    }
    std::size_t observed{};
    std::array<char, 64U> path{};
    while (const auto *entry = readdir(directory)) {
      const std::string_view name{entry->d_name};
      if (name.empty() || name.find_first_not_of("0123456789") !=
                              std::string_view::npos) {
        continue;
      }
      std::uint32_t pid{};
      const auto parsed_pid = std::from_chars(name.data(), name.data() + name.size(), pid);
      if (parsed_pid.ec != std::errc{} || parsed_pid.ptr != name.data() + name.size()) continue;
      if (destination.process_diagnostics.enumerated != UINT32_MAX) {
        ++destination.process_diagnostics.enumerated;
      }
      if (destination.processes.size() >= maximum_processes) break;
      if (!process_path(pid, "stat", path)) continue;
      const auto stat_read = read_bounded(path.data(), contents);
      if (stat_read != ReadStatus::available) {
        if (stat_read == ReadStatus::missing) {
          if (destination.process_diagnostics.exited_during_sample != UINT32_MAX)
            ++destination.process_diagnostics.exited_during_sample;
        } else if (destination.process_diagnostics.inaccessible != UINT32_MAX) {
          ++destination.process_diagnostics.inaccessible;
        }
        continue;
      }
      const auto stat = parse_proc_pid_stat(contents, pid);
      if (!stat) continue;
      ++observed;

      RawProcessCounters counters{};
      counters.identity = stat->identity;
      std::chrono::nanoseconds cpu{};
      counters.cpu_time = ticks_to_nanoseconds(stat->cpu_ticks, ticks_per_second, cpu)
                              ? MetricValue<std::chrono::nanoseconds>::available(cpu)
                              : MetricValue<std::chrono::nanoseconds>::unavailable(
                                    MetricStatus::temporarily_unavailable);
      counters.working_set = MetricValue<ByteCount>::unavailable(
          MetricStatus::temporarily_unavailable);
      counters.disk_read_bytes = MetricValue<ByteCount>::unavailable(
          MetricStatus::temporarily_unavailable);
      counters.disk_write_bytes = MetricValue<ByteCount>::unavailable(
          MetricStatus::temporarily_unavailable);

      if (process_path(pid, "status", path)) {
        const auto status_read = read_bounded(path.data(), contents);
        const auto memory = status_read == ReadStatus::available
                                ? parse_proc_pid_status_memory(contents)
                                : std::expected<ByteCount, ProcParseError>{
                                      std::unexpected{ProcParseError::missing_field}};
        counters.working_set = memory
                                   ? MetricValue<ByteCount>::available(*memory)
                                   : MetricValue<ByteCount>::unavailable(
                                         metric_status(status_read));
      }

      if (process_path(pid, "io", path)) {
        const auto io_read = read_bounded(path.data(), contents);
        const auto io = io_read == ReadStatus::available
                            ? parse_proc_pid_io(contents)
                            : std::expected<ProcProcessIo, ProcParseError>{
                                  std::unexpected{ProcParseError::missing_field}};
        counters.disk_read_bytes = io
                                       ? MetricValue<ByteCount>::available(io->read_bytes)
                                       : MetricValue<ByteCount>::unavailable(metric_status(io_read));
        counters.disk_write_bytes = io
                                        ? MetricValue<ByteCount>::available(io->write_bytes)
                                        : MetricValue<ByteCount>::unavailable(metric_status(io_read));
      }
      destination.processes.push_back(std::move(counters));
      if (destination.process_diagnostics.sampled != UINT32_MAX)
        ++destination.process_diagnostics.sampled;

      ProcessInfo info{};
      info.identity = stat->identity;
      info.parent_pid = MetricValue<ProcessId>::available(stat->parent_pid);
      info.name = MetricValue<std::string>::available(stat->name);
      info.executable_path = MetricValue<std::string>::unavailable(
          MetricStatus::temporarily_unavailable);
      if (resolve_paths) {
        MetricStatus path_status{};
        auto resolved = executable_path(pid, path_status);
        info.executable_path = path_status == MetricStatus::available
                                   ? MetricValue<std::string>::available(std::move(resolved))
                                   : MetricValue<std::string>::unavailable(path_status);
        if (path_status == MetricStatus::available) {
          if (destination.process_diagnostics.metadata_resolved != UINT32_MAX)
            ++destination.process_diagnostics.metadata_resolved;
        } else if (destination.process_diagnostics.metadata_failures != UINT32_MAX) {
          ++destination.process_diagnostics.metadata_failures;
        }
      }
      destination.process_metadata.push_back(std::move(info));
    }
    static_cast<void>(closedir(directory));
    active_count = observed;
    return destination.processes.empty() ? MetricStatus::temporarily_unavailable
                                         : MetricStatus::available;
  }

  std::uint64_t ticks_per_second{};
  std::size_t active_count{};
  std::string contents{};
};

LinuxProcessCollector::LinuxProcessCollector() noexcept
    : state_{std::make_unique<State>()} {}

LinuxProcessCollector::~LinuxProcessCollector() = default;

MetricStatus LinuxProcessCollector::collect(const bool resolve_paths,
                                            RawTelemetrySnapshot &destination) {
  return state_ != nullptr ? state_->collect(resolve_paths, destination)
                           : MetricStatus::temporarily_unavailable;
}

std::size_t LinuxProcessCollector::cache_size() const noexcept {
  return state_ != nullptr ? state_->active_count : 0U;
}

} // namespace blackbox::telemetry::linux
