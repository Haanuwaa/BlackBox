#include "telemetry/linux/linux_telemetry_provider.hpp"

#include "telemetry/linux/linux_proc_parser.hpp"
#include "telemetry/linux/linux_process_collector.hpp"
#include "telemetry/io_counter_tracker.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

namespace blackbox::telemetry::linux {
namespace {

constexpr std::size_t maximum_proc_file_bytes = 1024U * 1024U;
constexpr std::size_t maximum_io_entities = 128U;

[[nodiscard]] bool read_bounded_proc_file(const char *path,
                                          std::string &destination) {
  std::ifstream input{path, std::ios::binary};
  if (!input)
    return false;
  destination.clear();
  std::array<char, 4096U> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count <= 0)
      break;
    if (destination.size() + static_cast<std::size_t>(count) >
        maximum_proc_file_bytes) {
      destination.clear();
      return false;
    }
    destination.append(buffer.data(), static_cast<std::size_t>(count));
  }
  return input.eof() && !destination.empty();
}

template <typename T> [[nodiscard]] MetricValue<T> temporary() noexcept {
  return MetricValue<T>::unavailable(MetricStatus::temporarily_unavailable);
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

} // namespace

struct LinuxTelemetryProvider::NativeState {
  NativeState() {
    system_contents.reserve(64U * 1024U);
    disk_contents.reserve(4U * 1024U);
    network_contents.reserve(64U * 1024U);
  }

  [[nodiscard]] MetricStatus read_disks() {
    std::error_code error{};
    std::size_t count{};
    const std::filesystem::path root{"/sys/block"};
    for (std::filesystem::directory_iterator iterator{root, error}, end;
         !error && iterator != end; iterator.increment(error)) {
      if (count == disks.size()) return MetricStatus::temporarily_unavailable;
      const auto device = iterator->path() / "device";
      if (!std::filesystem::exists(device, error)) {
        error.clear();
        continue;
      }
      const auto stat = iterator->path() / "stat";
      if (!read_bounded_proc_file(stat.c_str(), disk_contents)) {
        return MetricStatus::temporarily_unavailable;
      }
      const auto parsed = parse_sys_block_stat(disk_contents);
      if (!parsed) return MetricStatus::temporarily_unavailable;
      const auto name = iterator->path().filename().string();
      disks[count++] = IoEntityCounters{stable_identity(name),
                                        parsed->read_bytes,
                                        parsed->write_bytes};
    }
    if (error || count == 0U) return MetricStatus::temporarily_unavailable;
    const auto totals = disk_tracker.update(
        std::span<const IoEntityCounters>{disks.data(), count});
    disk_read = totals.first;
    disk_write = totals.second;
    return disk_read.status == MetricStatus::available &&
                   disk_write.status == MetricStatus::available
               ? MetricStatus::available
               : MetricStatus::temporarily_unavailable;
  }

  [[nodiscard]] MetricStatus read_network() {
    if (!read_bounded_proc_file("/proc/net/dev", network_contents)) {
      return MetricStatus::temporarily_unavailable;
    }
    const auto count = parse_proc_net_dev(network_contents, interfaces);
    if (!count) return MetricStatus::temporarily_unavailable;
    const auto totals = network_tracker.update(
        std::span<const IoEntityCounters>{interfaces.data(), *count});
    network_receive = totals.first;
    network_transmit = totals.second;
    return network_receive.status == MetricStatus::available &&
                   network_transmit.status == MetricStatus::available
               ? MetricStatus::available
               : MetricStatus::temporarily_unavailable;
  }

  std::array<IoEntityCounters, maximum_io_entities> disks{};
  std::array<IoEntityCounters, maximum_io_entities> interfaces{};
  IoCounterTracker<maximum_io_entities> disk_tracker{};
  IoCounterTracker<maximum_io_entities> network_tracker{};
  LinuxProcessCollector process_collector{};
  MetricValue<ByteCount> disk_read{};
  MetricValue<ByteCount> disk_write{};
  MetricValue<ByteCount> network_receive{};
  MetricValue<ByteCount> network_transmit{};
  std::string system_contents{};
  std::string disk_contents{};
  std::string network_contents{};
};

LinuxTelemetryProvider::LinuxTelemetryProvider(
    const core::IMonotonicClock &clock) noexcept
    : clock_{clock}, native_state_{std::make_unique<NativeState>()} {}

LinuxTelemetryProvider::~LinuxTelemetryProvider() = default;

ProviderSampleResult
LinuxTelemetryProvider::sample(const SamplingRequest request,
                               RawTelemetrySnapshot &destination) {
  destination.reset(clock_.now(), request.tiers);
  destination.system.cpu_time = temporary<CpuTimeCounters>();
  destination.system.logical_processor_count = temporary<std::uint32_t>();
  destination.system.memory_total = temporary<ByteCount>();
  destination.system.memory_available = temporary<ByteCount>();
  destination.system.disk_read_bytes = temporary<ByteCount>();
  destination.system.disk_write_bytes = temporary<ByteCount>();
  destination.system.network_receive_bytes = temporary<ByteCount>();
  destination.system.network_transmit_bytes = temporary<ByteCount>();
  std::uint32_t attempted{};
  std::uint32_t failed{};
  auto &contents = native_state_->system_contents;

  if (request.tiers.contains(SamplingTier::fast)) {
    ++attempted;
    if (read_bounded_proc_file("/proc/stat", contents)) {
      const auto parsed = parse_proc_stat(contents);
      if (parsed) {
        destination.system.cpu_time =
            MetricValue<CpuTimeCounters>::available(parsed->counters);
        destination.system.logical_processor_count =
            MetricValue<std::uint32_t>::available(
                parsed->logical_processor_count);
      } else {
        ++failed;
      }
    } else {
      ++failed;
    }
    ++attempted;
    if (native_state_ != nullptr &&
        native_state_->read_disks() == MetricStatus::available) {
      destination.system.disk_read_bytes = native_state_->disk_read;
      destination.system.disk_write_bytes = native_state_->disk_write;
    } else {
      ++failed;
    }
    ++attempted;
    if (native_state_ != nullptr &&
        native_state_->read_network() == MetricStatus::available) {
      destination.system.network_receive_bytes = native_state_->network_receive;
      destination.system.network_transmit_bytes = native_state_->network_transmit;
    } else {
      ++failed;
    }
  }

  if (request.tiers.contains(SamplingTier::normal)) {
    ++attempted;
    if (read_bounded_proc_file("/proc/meminfo", contents)) {
      const auto parsed = parse_proc_meminfo(contents);
      if (parsed) {
        destination.system.memory_total =
            MetricValue<ByteCount>::available(parsed->total);
        destination.system.memory_available =
            MetricValue<ByteCount>::available(parsed->available);
      } else {
        ++failed;
      }
    } else {
      ++failed;
    }
    ++attempted;
    if (native_state_ == nullptr ||
        native_state_->process_collector.collect(
            request.tiers.contains(SamplingTier::slow), destination) !=
            MetricStatus::available) {
      ++failed;
    }
  }

  const auto status = failed == 0U ? ProviderSampleStatus::complete
                      : failed == attempted
                          ? ProviderSampleStatus::temporarily_failed
                          : ProviderSampleStatus::partial;
  return ProviderSampleResult{status, ++sequence_};
}

PlatformCapabilities LinuxTelemetryProvider::capabilities() const noexcept {
  PlatformCapabilities result{};
  result.cpu_usage = true;
  result.memory_usage = true;
  result.process_cpu = true;
  result.process_memory = true;
  result.process_disk_io = true;
  result.disk_throughput = true;
  result.network_usage = true;
  return result;
}

} // namespace blackbox::telemetry::linux
