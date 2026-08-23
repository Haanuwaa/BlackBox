#include "telemetry/linux/linux_telemetry_provider.hpp"

#include "telemetry/linux/linux_proc_parser.hpp"

#include <array>
#include <fstream>
#include <string>
#include <string_view>

namespace blackbox::telemetry::linux {
namespace {

constexpr std::size_t maximum_proc_file_bytes = 1024U * 1024U;

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

} // namespace

LinuxTelemetryProvider::LinuxTelemetryProvider(
    const core::IMonotonicClock &clock) noexcept
    : clock_{clock} {}

ProviderSampleResult
LinuxTelemetryProvider::sample(const SamplingRequest request,
                               RawTelemetrySnapshot &destination) {
  destination.reset(clock_.now(), request.tiers);
  destination.system.cpu_time = temporary<CpuTimeCounters>();
  destination.system.logical_processor_count = temporary<std::uint32_t>();
  destination.system.memory_total = temporary<ByteCount>();
  destination.system.memory_available = temporary<ByteCount>();
  std::uint32_t attempted{};
  std::uint32_t failed{};
  std::string contents{};

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
  return result;
}

} // namespace blackbox::telemetry::linux
