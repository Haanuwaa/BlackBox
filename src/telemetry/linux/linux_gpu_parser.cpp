#include "telemetry/linux/linux_gpu_parser.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>

namespace blackbox::telemetry::linux {
namespace {

constexpr std::size_t maximum_fdinfo_bytes = 64U * 1024U;
constexpr std::size_t maximum_fdinfo_lines = 512U;
constexpr std::size_t maximum_engines = 64U;

[[nodiscard]] constexpr std::string_view trim(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                            value.front() == '\r' || value.front() == '\n'))
    value.remove_prefix(1U);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                            value.back() == '\r' || value.back() == '\n'))
    value.remove_suffix(1U);
  return value;
}

[[nodiscard]] std::optional<std::uint64_t>
unsigned_prefix(const std::string_view input) noexcept {
  const auto value = trim(input);
  if (value.empty())
    return std::nullopt;
  std::uint64_t result{};
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr == value.data())
    return std::nullopt;
  return result;
}

[[nodiscard]] constexpr std::uint64_t
hash_append(std::uint64_t result, const std::string_view value) noexcept {
  for (const auto byte : value) {
    result ^= static_cast<unsigned char>(byte);
    result *= 1099511628211ULL;
  }
  return result;
}

[[nodiscard]] constexpr std::uint64_t
hash_number(std::uint64_t result, const std::uint64_t value) noexcept {
  for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
    result ^= static_cast<unsigned char>(value >> shift);
    result *= 1099511628211ULL;
  }
  return result;
}

[[nodiscard]] std::optional<std::uint64_t>
scaled_bytes(const std::string_view input) noexcept {
  const auto value = trim(input);
  const auto number = unsigned_prefix(value);
  if (!number)
    return std::nullopt;
  const auto separator = value.find_first_of(" \t");
  const auto unit = separator == std::string_view::npos
                        ? std::string_view{}
                        : trim(value.substr(separator));
  std::uint64_t scale{1U};
  if (unit == "KiB")
    scale = 1024U;
  else if (unit == "MiB")
    scale = 1024U * 1024U;
  else if (unit == "GiB")
    scale = 1024U * 1024U * 1024U;
  else if (!unit.empty() && unit != "B")
    return std::nullopt;
  if (*number > (std::numeric_limits<std::uint64_t>::max)() / scale) {
    return std::nullopt;
  }
  return *number * scale;
}

struct PartialEngine {
  std::string key{};
  std::optional<std::uint64_t> nanoseconds{};
  std::optional<std::uint64_t> cycles{};
  std::optional<std::uint64_t> total_cycles{};
  std::uint32_t capacity{1U};
};

[[nodiscard]] PartialEngine *
engine_for(std::array<PartialEngine, maximum_engines> &engines,
           std::size_t &count, const std::string_view key) {
  const auto found = std::find_if(
      engines.begin(), engines.begin() + static_cast<std::ptrdiff_t>(count),
      [key](const PartialEngine &value) { return value.key == key; });
  if (found != engines.begin() + static_cast<std::ptrdiff_t>(count))
    return &*found;
  if (count == engines.size() || key.empty() || key.size() > 64U)
    return nullptr;
  engines[count].key.assign(key);
  return &engines[count++];
}

} // namespace

std::optional<Ratio>
parse_gpu_busy_percent(const std::string_view contents) noexcept {
  const auto value = unsigned_prefix(contents);
  if (!value || *value > 100U)
    return std::nullopt;
  return Ratio{static_cast<double>(*value) / 100.0};
}

std::optional<ByteCount>
parse_gpu_memory_bytes(const std::string_view contents) noexcept {
  const auto value = scaled_bytes(contents);
  if (!value)
    return std::nullopt;
  return ByteCount{*value};
}

std::optional<DrmFdinfoEvidence>
parse_drm_fdinfo(const std::string_view contents,
                 const std::uint64_t identity_seed) {
  if (contents.empty() || contents.size() > maximum_fdinfo_bytes ||
      identity_seed == 0U) {
    return std::nullopt;
  }

  std::array<PartialEngine, maximum_engines> partial{};
  std::size_t engine_count{};
  std::string device_key{};
  std::uint64_t client_id{};
  bool has_client{};
  std::uint64_t resident_vram{};
  bool has_vram{};
  std::size_t lines{};
  std::size_t offset{};
  while (offset <= contents.size()) {
    if (++lines > maximum_fdinfo_lines)
      return std::nullopt;
    const auto end = contents.find('\n', offset);
    const auto line = trim(contents.substr(
        offset, end == std::string_view::npos ? contents.size() - offset
                                              : end - offset));
    const auto separator = line.find(':');
    if (separator != std::string_view::npos) {
      const auto key = trim(line.substr(0U, separator));
      const auto value = trim(line.substr(separator + 1U));
      if (key == "drm-pdev" || key == "drm-device") {
        if (value.empty() || value.size() > 128U)
          return std::nullopt;
        device_key.assign(value);
      } else if (key == "drm-client-id") {
        const auto parsed = unsigned_prefix(value);
        if (!parsed)
          return std::nullopt;
        client_id = *parsed;
        has_client = true;
      } else if (key.starts_with("drm-engine-capacity-")) {
        const auto parsed = unsigned_prefix(value);
        auto *engine = engine_for(partial, engine_count, key.substr(20U));
        if (!parsed || *parsed == 0U ||
            *parsed > (std::numeric_limits<std::uint32_t>::max)() ||
            engine == nullptr)
          return std::nullopt;
        engine->capacity = static_cast<std::uint32_t>(*parsed);
      } else if (key.starts_with("drm-total-cycles-")) {
        const auto parsed = unsigned_prefix(value);
        auto *engine = engine_for(partial, engine_count, key.substr(17U));
        if (!parsed || engine == nullptr)
          return std::nullopt;
        engine->total_cycles = *parsed;
      } else if (key.starts_with("drm-cycles-")) {
        const auto parsed = unsigned_prefix(value);
        auto *engine = engine_for(partial, engine_count, key.substr(11U));
        if (!parsed || engine == nullptr)
          return std::nullopt;
        engine->cycles = *parsed;
      } else if (key.starts_with("drm-engine-")) {
        const auto parsed = unsigned_prefix(value);
        auto *engine = engine_for(partial, engine_count, key.substr(11U));
        if (!parsed || engine == nullptr)
          return std::nullopt;
        engine->nanoseconds = *parsed;
      } else if (key.starts_with("drm-resident-vram")) {
        const auto parsed = scaled_bytes(value);
        if (!parsed || *parsed > (std::numeric_limits<std::uint64_t>::max)() -
                                     resident_vram) {
          return std::nullopt;
        }
        resident_vram += *parsed;
        has_vram = true;
      }
    }
    if (end == std::string_view::npos)
      break;
    offset = end + 1U;
  }
  if (device_key.empty() || !has_client || engine_count == 0U)
    return std::nullopt;

  DrmFdinfoEvidence result{};
  result.engines.reserve(engine_count);
  for (std::size_t index = 0U; index < engine_count; ++index) {
    const auto &engine = partial[index];
    const bool use_cycles =
        engine.cycles.has_value() && engine.total_cycles.has_value();
    if (!engine.nanoseconds && !use_cycles)
      continue;
    auto engine_identity = hash_append(14695981039346656037ULL, device_key);
    engine_identity = hash_append(engine_identity, engine.key);
    auto counter_identity = hash_number(engine_identity, identity_seed);
    counter_identity = hash_number(counter_identity, client_id);
    result.engines.push_back(DrmEngineCounter{
        counter_identity, engine_identity,
        use_cycles ? *engine.cycles : *engine.nanoseconds,
        use_cycles ? engine.total_cycles : std::nullopt, engine.capacity});
  }
  if (result.engines.empty())
    return std::nullopt;
  result.resident_vram =
      has_vram ? MetricValue<ByteCount>::available(ByteCount{resident_vram})
               : MetricValue<ByteCount>::unavailable(MetricStatus::unsupported);
  return result;
}

MetricValue<Ratio> DrmActivityTracker::update(
    const core::MonotonicTimePoint observed_at,
    const std::span<const DrmEngineCounter> counters) noexcept {
  if (++generation_ == 0U) {
    generation_ = 1U;
    for (auto &state : states_)
      state = {};
  }
  const auto elapsed =
      has_previous_observation_ && observed_at > previous_observation_
          ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                observed_at - previous_observation_)
                .count()
          : 0LL;
  previous_observation_ = observed_at;
  has_previous_observation_ = true;

  struct Group {
    std::uint64_t identity{};
    std::uint64_t busy_delta{};
    std::uint64_t total_delta{};
    std::uint32_t capacity{1U};
    bool has_total{};
  };
  std::array<Group, 64U> groups{};
  std::size_t group_count{};
  bool overflow{};
  bool comparable{};

  for (const auto &counter : counters) {
    if (counter.private_counter_identity == 0U ||
        counter.private_engine_identity == 0U || counter.capacity == 0U) {
      overflow = true;
      continue;
    }
    auto state = std::find_if(
        states_.begin(), states_.end(),
        [identity = counter.private_counter_identity](const State &value) {
          return value.identity == identity;
        });
    if (state == states_.end()) {
      state =
          std::find_if(states_.begin(), states_.end(),
                       [](const State &value) { return value.identity == 0U; });
      if (state == states_.end()) {
        overflow = true;
        continue;
      }
      state->identity = counter.private_counter_identity;
    } else if (counter.busy >= state->busy &&
               (!counter.total || !state->total ||
                *counter.total >= *state->total)) {
      auto group = std::find_if(
          groups.begin(),
          groups.begin() + static_cast<std::ptrdiff_t>(group_count),
          [identity = counter.private_engine_identity](const Group &value) {
            return value.identity == identity;
          });
      if (group == groups.begin() + static_cast<std::ptrdiff_t>(group_count)) {
        if (group_count == groups.size()) {
          overflow = true;
        } else {
          groups[group_count].identity = counter.private_engine_identity;
          group = groups.begin() + static_cast<std::ptrdiff_t>(group_count++);
        }
      }
      if (group != groups.begin() + static_cast<std::ptrdiff_t>(group_count)) {
        const auto delta = counter.busy - state->busy;
        if (delta >
            (std::numeric_limits<std::uint64_t>::max)() - group->busy_delta) {
          overflow = true;
        } else {
          group->busy_delta += delta;
          group->capacity = (std::max)(group->capacity, counter.capacity);
          if (counter.total && state->total) {
            group->has_total = true;
            group->total_delta =
                (std::max)(group->total_delta, *counter.total - *state->total);
          }
          comparable = true;
        }
      }
    }
    state->busy = counter.busy;
    state->total = counter.total;
    state->generation = generation_;
  }

  for (auto &state : states_) {
    if (state.identity != 0U && state.generation != generation_)
      state = {};
  }
  if (overflow) {
    return MetricValue<Ratio>::unavailable(
        MetricStatus::temporarily_unavailable);
  }
  if (!comparable || elapsed <= 0LL) {
    return MetricValue<Ratio>::unavailable(
        counters.empty() ? MetricStatus::unsupported
                         : MetricStatus::temporarily_unavailable);
  }

  double maximum{};
  bool has_ratio{};
  for (std::size_t index = 0U; index < group_count; ++index) {
    const auto &group = groups[index];
    std::uint64_t denominator{};
    if (group.has_total) {
      denominator = group.total_delta;
    } else if (static_cast<std::uint64_t>(elapsed) <=
               (std::numeric_limits<std::uint64_t>::max)() / group.capacity) {
      denominator = static_cast<std::uint64_t>(elapsed) * group.capacity;
    }
    if (denominator == 0U)
      continue;
    maximum = (std::max)(maximum, static_cast<double>(group.busy_delta) /
                                      static_cast<double>(denominator));
    has_ratio = true;
  }
  return has_ratio ? MetricValue<Ratio>::available(
                         Ratio{std::clamp(maximum, 0.0, 1.0)})
                   : MetricValue<Ratio>::unavailable(
                         MetricStatus::temporarily_unavailable);
}

} // namespace blackbox::telemetry::linux
