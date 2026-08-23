#pragma once

#include "telemetry/types.hpp"

#include <cstddef>
#include <memory>

namespace blackbox::telemetry::linux {

class LinuxProcessCollector final {
public:
  LinuxProcessCollector() noexcept;
  ~LinuxProcessCollector();

  LinuxProcessCollector(const LinuxProcessCollector &) = delete;
  LinuxProcessCollector &operator=(const LinuxProcessCollector &) = delete;

  [[nodiscard]] MetricStatus collect(bool resolve_paths,
                                     RawTelemetrySnapshot &destination);
  [[nodiscard]] std::size_t cache_size() const noexcept;

private:
  struct State;
  std::unique_ptr<State> state_{};
};

} // namespace blackbox::telemetry::linux
