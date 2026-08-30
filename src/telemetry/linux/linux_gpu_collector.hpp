#pragma once

#include "core/clock.hpp"
#include "telemetry/gpu_aggregation.hpp"
#include "telemetry/linux/linux_gpu_parser.hpp"

#include <filesystem>
#include <memory>
#include <optional>

namespace blackbox::telemetry::linux {

struct LinuxGpuPaths {
  std::filesystem::path drm_root{"/sys/class/drm"};
  std::filesystem::path proc_root{"/proc"};
  bool enable_nvml{true};
  std::filesystem::path nvml_library{};
};

struct LinuxGpuReading {
  GpuAggregateReading system{};
  MetricValue<Ratio> foreground_usage{};
};

class LinuxGpuCollector final {
public:
  explicit LinuxGpuCollector(LinuxGpuPaths paths = {});
  ~LinuxGpuCollector();
  LinuxGpuCollector(const LinuxGpuCollector &) = delete;
  LinuxGpuCollector &operator=(const LinuxGpuCollector &) = delete;

  [[nodiscard]] LinuxGpuReading
  collect(core::MonotonicTimePoint observed_at,
          std::optional<ProcessIdentity> foreground_process,
          bool refresh_inventory);
  [[nodiscard]] GpuInventoryEvidence inventory() const noexcept;
  [[nodiscard]] bool supports_system_usage() const noexcept;
  [[nodiscard]] bool supports_memory() const noexcept;
  [[nodiscard]] bool supports_foreground_usage() const noexcept;

private:
  struct NativeState;
  std::unique_ptr<NativeState> native_state_{};
};

} // namespace blackbox::telemetry::linux
