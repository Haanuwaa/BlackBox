#pragma once

#include "core/clock.hpp"
#include "telemetry/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace blackbox::telemetry::linux {

[[nodiscard]] std::optional<Ratio>
parse_gpu_busy_percent(std::string_view contents) noexcept;
[[nodiscard]] std::optional<ByteCount>
parse_gpu_memory_bytes(std::string_view contents) noexcept;

struct DrmEngineCounter {
  std::uint64_t private_counter_identity{};
  std::uint64_t private_engine_identity{};
  std::uint64_t busy{};
  std::optional<std::uint64_t> total{};
  std::uint32_t capacity{1U};
  friend constexpr bool operator==(const DrmEngineCounter &,
                                   const DrmEngineCounter &) = default;
};

struct DrmFdinfoEvidence {
  std::vector<DrmEngineCounter> engines{};
  MetricValue<ByteCount> resident_vram{};
};

// The identity seed is the process creation token. Parsed output contains only
// one-way private hashes; fdinfo labels, PCI locations, and client ids are not
// retained.
[[nodiscard]] std::optional<DrmFdinfoEvidence>
parse_drm_fdinfo(std::string_view contents, std::uint64_t identity_seed);

class DrmActivityTracker {
public:
  [[nodiscard]] MetricValue<Ratio>
  update(core::MonotonicTimePoint observed_at,
         std::span<const DrmEngineCounter> counters) noexcept;

private:
  struct State {
    std::uint64_t identity{};
    std::uint64_t busy{};
    std::optional<std::uint64_t> total{};
    std::uint64_t generation{};
  };
  std::array<State, 256U> states_{};
  core::MonotonicTimePoint previous_observation_{};
  bool has_previous_observation_{};
  std::uint64_t generation_{};
};

} // namespace blackbox::telemetry::linux
