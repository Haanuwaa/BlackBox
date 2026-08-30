#include "telemetry/linux/linux_gpu_collector.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace blackbox::telemetry::linux {
namespace {

constexpr std::size_t maximum_gpu_devices = 32U;
constexpr std::size_t maximum_fdinfo_files = 1024U;
constexpr std::size_t maximum_file_bytes = 64U * 1024U;
constexpr int nvml_success = 0;

[[nodiscard]] constexpr std::uint64_t
stable_identity(const std::string_view value) noexcept {
  std::uint64_t result{14695981039346656037ULL};
  for (const auto byte : value) {
    result ^= static_cast<unsigned char>(byte);
    result *= 1099511628211ULL;
  }
  return result;
}

[[nodiscard]] MetricStatus read_bounded_file(const std::filesystem::path &path,
                                             std::string &destination) {
  errno = 0;
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return errno == EACCES || errno == EPERM
               ? MetricStatus::inaccessible
               : MetricStatus::temporarily_unavailable;
  }
  destination.clear();
  std::array<char, 4096U> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count <= 0)
      break;
    if (destination.size() + static_cast<std::size_t>(count) >
        maximum_file_bytes) {
      destination.clear();
      return MetricStatus::temporarily_unavailable;
    }
    destination.append(buffer.data(), static_cast<std::size_t>(count));
  }
  return input.eof() && !destination.empty()
             ? MetricStatus::available
             : MetricStatus::temporarily_unavailable;
}

struct NvmlDeviceOpaque;
using NvmlDevice = NvmlDeviceOpaque *;
struct NvmlUtilization {
  unsigned int gpu{};
  unsigned int memory{};
};
struct NvmlMemory {
  unsigned long long total{};
  unsigned long long free{};
  unsigned long long used{};
};

class NvmlReader final {
public:
  explicit NvmlReader(const bool enabled = true,
                      const std::filesystem::path &library_path = {}) noexcept {
    if (!enabled)
      return;
    library_ = dlopen(library_path.empty() ? "libnvidia-ml.so.1"
                                           : library_path.c_str(),
                      RTLD_LAZY | RTLD_LOCAL);
    if (library_ == nullptr)
      return;
    init_ = symbol<Init>("nvmlInit_v2");
    shutdown_ = symbol<Shutdown>("nvmlShutdown");
    count_ = symbol<DeviceCount>("nvmlDeviceGetCount_v2");
    handle_ = symbol<DeviceHandle>("nvmlDeviceGetHandleByIndex_v2");
    utilization_ = symbol<DeviceUtilization>("nvmlDeviceGetUtilizationRates");
    memory_ = symbol<DeviceMemory>("nvmlDeviceGetMemoryInfo");
    if (init_ == nullptr || shutdown_ == nullptr || count_ == nullptr ||
        handle_ == nullptr || utilization_ == nullptr || memory_ == nullptr ||
        init_() != nvml_success) {
      close();
      return;
    }
    initialized_ = true;
  }

  ~NvmlReader() { close(); }
  NvmlReader(const NvmlReader &) = delete;
  NvmlReader &operator=(const NvmlReader &) = delete;

  [[nodiscard]] bool available() const noexcept { return initialized_; }

  [[nodiscard]] std::size_t
  append(std::span<GpuDeviceReading> destination) noexcept {
    if (!initialized_)
      return 0U;
    unsigned int count{};
    if (count_(&count) != nvml_success)
      return 0U;
    const auto bounded =
        (std::min)(static_cast<std::size_t>(count), destination.size());
    std::size_t written{};
    for (std::size_t index = 0U; index < bounded; ++index) {
      NvmlDevice device{};
      const auto native_index = static_cast<unsigned int>(index);
      if (handle_(native_index, &device) != nvml_success || device == nullptr)
        continue;
      NvmlUtilization utilization{};
      NvmlMemory memory{};
      const bool has_utilization =
          utilization_(device, &utilization) == nvml_success &&
          utilization.gpu <= 100U;
      const bool has_memory = memory_(device, &memory) == nvml_success &&
                              memory.used <= memory.total;
      destination[written++] = GpuDeviceReading{
          0x4e564d4c00000001ULL + index,
          has_utilization ? MetricValue<Ratio>::available(Ratio{
                                static_cast<double>(utilization.gpu) / 100.0})
                          : MetricValue<Ratio>::unavailable(
                                MetricStatus::temporarily_unavailable),
          has_memory ? MetricValue<ByteCount>::available(
                           ByteCount{static_cast<std::uint64_t>(memory.used)})
                     : MetricValue<ByteCount>::unavailable(
                           MetricStatus::temporarily_unavailable)};
    }
    device_count_ = static_cast<std::uint32_t>(bounded);
    return written;
  }

  [[nodiscard]] std::uint32_t device_count() const noexcept {
    return device_count_;
  }

private:
  using Init = int (*)();
  using Shutdown = int (*)();
  using DeviceCount = int (*)(unsigned int *);
  using DeviceHandle = int (*)(unsigned int, NvmlDevice *);
  using DeviceUtilization = int (*)(NvmlDevice, NvmlUtilization *);
  using DeviceMemory = int (*)(NvmlDevice, NvmlMemory *);

  template <typename Function>
  [[nodiscard]] Function symbol(const char *name) noexcept {
    const auto address = dlsym(library_, name);
    Function result{};
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
  }

  void close() noexcept {
    if (initialized_ && shutdown_ != nullptr)
      static_cast<void>(shutdown_());
    initialized_ = false;
    if (library_ != nullptr)
      dlclose(library_);
    library_ = nullptr;
  }

  void *library_{};
  Init init_{};
  Shutdown shutdown_{};
  DeviceCount count_{};
  DeviceHandle handle_{};
  DeviceUtilization utilization_{};
  DeviceMemory memory_{};
  bool initialized_{};
  std::uint32_t device_count_{};
};

} // namespace

struct LinuxGpuCollector::NativeState {
  explicit NativeState(LinuxGpuPaths value)
      : paths{std::move(value)}, nvml{paths.enable_nvml, paths.nvml_library} {
    contents.reserve(maximum_file_bytes);
    refresh();
  }

  struct Device {
    std::uint64_t identity{};
    std::filesystem::path device_path{};
    bool amd{};
    bool nvidia{};
    bool integrated{};
  };

  void refresh() {
    devices.fill({});
    device_count = 0U;
    render_device_available = false;
    inventory_status = MetricStatus::available;
    std::error_code error{};
    if (!std::filesystem::exists(paths.drm_root, error) || error) {
      inventory_status = error == std::errc::permission_denied
                             ? MetricStatus::inaccessible
                             : MetricStatus::temporarily_unavailable;
      return;
    }
    for (std::filesystem::directory_iterator iterator{paths.drm_root, error},
         end;
         !error && iterator != end; iterator.increment(error)) {
      const auto name = iterator->path().filename().string();
      if (name.starts_with("renderD"))
        render_device_available = true;
      if (!name.starts_with("card") || name.find('-') != std::string::npos ||
          name.size() <= 4U ||
          !std::all_of(name.begin() + 4, name.end(), [](const char value) {
            return value >= '0' && value <= '9';
          })) {
        continue;
      }
      if (device_count == devices.size()) {
        inventory_status = MetricStatus::temporarily_unavailable;
        return;
      }
      const auto device_path = iterator->path() / "device";
      if (!std::filesystem::exists(device_path, error)) {
        error.clear();
        continue;
      }
      auto driver_name = std::string{};
      const auto driver =
          std::filesystem::read_symlink(device_path / "driver", error);
      if (!error)
        driver_name = driver.filename().string();
      error.clear();
      const bool amd = driver_name == "amdgpu";
      const bool nvidia = driver_name == "nvidia";
      const bool integrated = driver_name == "i915" || driver_name == "xe" ||
                              driver_name == "panfrost" ||
                              driver_name == "panthor";
      devices[device_count++] =
          Device{stable_identity(iterator->path().string()), device_path, amd,
                 nvidia, integrated};
    }
    if (error) {
      inventory_status = error == std::errc::permission_denied
                             ? MetricStatus::inaccessible
                             : MetricStatus::temporarily_unavailable;
    }
  }

  [[nodiscard]] LinuxGpuReading
  collect(const core::MonotonicTimePoint observed_at,
          const std::optional<ProcessIdentity> foreground) {
    std::array<GpuDeviceReading, maximum_gpu_devices> readings{};
    std::size_t reading_count{};
    for (std::size_t index = 0U; index < device_count; ++index) {
      const auto &device = devices[index];
      if (!device.amd || reading_count == readings.size())
        continue;
      const auto usage_status =
          read_bounded_file(device.device_path / "gpu_busy_percent", contents);
      const auto usage = usage_status == MetricStatus::available
                             ? parse_gpu_busy_percent(contents)
                             : std::nullopt;
      const auto memory_status = read_bounded_file(
          device.device_path / "mem_info_vram_used", contents);
      const auto memory = memory_status == MetricStatus::available
                              ? parse_gpu_memory_bytes(contents)
                              : std::nullopt;
      readings[reading_count++] = GpuDeviceReading{
          device.identity,
          usage ? MetricValue<Ratio>::available(*usage)
                : MetricValue<Ratio>::unavailable(
                      usage_status == MetricStatus::available
                          ? MetricStatus::temporarily_unavailable
                          : usage_status),
          memory ? MetricValue<ByteCount>::available(*memory)
                 : MetricValue<ByteCount>::unavailable(
                       memory_status == MetricStatus::available
                           ? MetricStatus::temporarily_unavailable
                           : memory_status)};
    }
    if (reading_count < readings.size()) {
      reading_count += nvml.append(std::span<GpuDeviceReading>{
          readings.data() + static_cast<std::ptrdiff_t>(reading_count),
          readings.size() - reading_count});
    }

    LinuxGpuReading result{};
    result.system = aggregate_gpu_devices(
        std::span<const GpuDeviceReading>{readings.data(), reading_count});
    result.foreground_usage = collect_foreground(observed_at, foreground);
    return result;
  }

  [[nodiscard]] MetricValue<Ratio>
  collect_foreground(const core::MonotonicTimePoint observed_at,
                     const std::optional<ProcessIdentity> foreground) {
    if (!foreground) {
      return MetricValue<Ratio>::unavailable(MetricStatus::unsupported);
    }
    const auto root =
        paths.proc_root / std::to_string(foreground->pid.value) / "fdinfo";
    std::error_code error{};
    std::filesystem::directory_iterator iterator{root, error};
    if (error) {
      return MetricValue<Ratio>::unavailable(
          error == std::errc::permission_denied
              ? MetricStatus::inaccessible
              : MetricStatus::temporarily_unavailable);
    }
    std::vector<DrmEngineCounter> counters{};
    counters.reserve(64U);
    std::size_t files{};
    for (std::filesystem::directory_iterator end; !error && iterator != end;
         iterator.increment(error)) {
      if (++files > maximum_fdinfo_files) {
        return MetricValue<Ratio>::unavailable(
            MetricStatus::temporarily_unavailable);
      }
      const auto status = read_bounded_file(iterator->path(), contents);
      if (status == MetricStatus::inaccessible) {
        return MetricValue<Ratio>::unavailable(status);
      }
      if (status != MetricStatus::available)
        continue;
      const auto parsed =
          parse_drm_fdinfo(contents, foreground->creation_token);
      if (!parsed)
        continue;
      if (counters.size() + parsed->engines.size() > 256U) {
        return MetricValue<Ratio>::unavailable(
            MetricStatus::temporarily_unavailable);
      }
      counters.insert(counters.end(), parsed->engines.begin(),
                      parsed->engines.end());
    }
    if (error) {
      return MetricValue<Ratio>::unavailable(
          error == std::errc::permission_denied
              ? MetricStatus::inaccessible
              : MetricStatus::temporarily_unavailable);
    }
    return drm_tracker.update(observed_at, counters);
  }

  [[nodiscard]] GpuInventoryEvidence inventory() const noexcept {
    GpuInventoryEvidence result{};
    if (inventory_status != MetricStatus::available) {
      result.device_count =
          MetricValue<std::uint32_t>::unavailable(inventory_status);
      result.integrated_device_count =
          MetricValue<std::uint32_t>::unavailable(inventory_status);
      result.discrete_device_count =
          MetricValue<std::uint32_t>::unavailable(inventory_status);
      result.render_device_available =
          MetricValue<bool>::unavailable(inventory_status);
      return result;
    }
    std::uint32_t integrated{};
    std::uint32_t discrete{};
    bool has_nvidia{};
    for (std::size_t index = 0U; index < device_count; ++index) {
      integrated += devices[index].integrated ? 1U : 0U;
      discrete += devices[index].integrated ? 0U : 1U;
      has_nvidia = has_nvidia || devices[index].nvidia;
    }
    auto total = static_cast<std::uint32_t>(device_count);
    if (!has_nvidia && nvml.available())
      total += nvml.device_count();
    result.device_count = MetricValue<std::uint32_t>::available(total);
    result.integrated_device_count =
        MetricValue<std::uint32_t>::available(integrated);
    result.discrete_device_count = MetricValue<std::uint32_t>::available(
        discrete +
        (!has_nvidia && nvml.available() ? nvml.device_count() : 0U));
    result.render_device_available =
        MetricValue<bool>::available(render_device_available);
    return result;
  }

  [[nodiscard]] bool has_amd() const noexcept {
    return std::any_of(devices.begin(),
                       devices.begin() +
                           static_cast<std::ptrdiff_t>(device_count),
                       [](const Device &value) { return value.amd; });
  }

  LinuxGpuPaths paths{};
  std::array<Device, maximum_gpu_devices> devices{};
  std::size_t device_count{};
  bool render_device_available{};
  MetricStatus inventory_status{MetricStatus::temporarily_unavailable};
  std::string contents{};
  NvmlReader nvml{};
  DrmActivityTracker drm_tracker{};
};

LinuxGpuCollector::LinuxGpuCollector(LinuxGpuPaths paths)
    : native_state_{std::make_unique<NativeState>(std::move(paths))} {}

LinuxGpuCollector::~LinuxGpuCollector() = default;

LinuxGpuReading LinuxGpuCollector::collect(
    const core::MonotonicTimePoint observed_at,
    const std::optional<ProcessIdentity> foreground_process,
    const bool refresh_inventory) {
  if (refresh_inventory)
    native_state_->refresh();
  return native_state_->collect(observed_at, foreground_process);
}

GpuInventoryEvidence LinuxGpuCollector::inventory() const noexcept {
  return native_state_->inventory();
}

bool LinuxGpuCollector::supports_system_usage() const noexcept {
  return native_state_->has_amd() || native_state_->nvml.available();
}

bool LinuxGpuCollector::supports_memory() const noexcept {
  return supports_system_usage();
}

bool LinuxGpuCollector::supports_foreground_usage() const noexcept {
  return native_state_->device_count != 0U;
}

} // namespace blackbox::telemetry::linux
