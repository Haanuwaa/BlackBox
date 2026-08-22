#include "telemetry/process_normalizer.hpp"

#include "telemetry/normalizer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace blackbox::telemetry {
namespace {

template <typename T>
[[nodiscard]] MetricValue<T> unavailable(const MetricStatus status) noexcept {
    return MetricValue<T>::unavailable(status);
}

} // namespace

MetricValue<Ratio> normalize_process_cpu(
    const MetricValue<std::chrono::nanoseconds>& previous,
    const MetricValue<std::chrono::nanoseconds>& current,
    const std::chrono::steady_clock::duration elapsed,
    const MetricValue<std::uint32_t>& logical_processor_count) noexcept {
    if (!current.has_value()) {
        return unavailable<Ratio>(current.status);
    }
    if (!previous.has_value() || !logical_processor_count.has_value()) {
        return unavailable<Ratio>(MetricStatus::temporarily_unavailable);
    }
    if (logical_processor_count.value == 0U ||
        elapsed <= std::chrono::steady_clock::duration::zero() ||
        current.value < previous.value) {
        return unavailable<Ratio>(MetricStatus::temporarily_unavailable);
    }
    const std::chrono::duration<double> cpu_delta{current.value - previous.value};
    const std::chrono::duration<double> wall_delta{elapsed};
    const auto usage = cpu_delta.count() /
                       (wall_delta.count() * logical_processor_count.value);
    if (!std::isfinite(usage) || usage < 0.0 || usage > 1.01) {
        return unavailable<Ratio>(MetricStatus::temporarily_unavailable);
    }
    return MetricValue<Ratio>::available(Ratio{std::clamp(usage, 0.0, 1.0)});
}

std::size_t ProcessTelemetryNormalizer::IdentityHash::operator()(
    const ProcessIdentity& identity) const noexcept {
    const auto pid = static_cast<std::uint64_t>(identity.pid.value);
    const auto mixed = identity.creation_token ^ (pid + 0x9e3779b97f4a7c15ULL +
                                                    (identity.creation_token << 6U) +
                                                    (identity.creation_token >> 2U));
    return static_cast<std::size_t>(mixed);
}

void ProcessTelemetryNormalizer::normalize(
    const RawTelemetrySnapshot& raw,
    std::vector<ProcessSample>& destination) {
    destination.clear();
    if (!raw.sampled_tiers.contains(SamplingTier::normal)) {
        return;
    }
    destination.reserve(raw.processes.size());
    ++generation_;

    for (const auto& current : raw.processes) {
        ProcessSample sample{};
        sample.identity = current.identity;
        sample.working_set = current.working_set;

        const auto previous = previous_.find(current.identity);
        if (previous == previous_.end()) {
            sample.cpu_usage = unavailable<Ratio>(
                current.cpu_time.has_value() ? MetricStatus::temporarily_unavailable
                                             : current.cpu_time.status);
            sample.disk_read_rate = unavailable<BytesPerSecond>(
                current.disk_read_bytes.has_value() ? MetricStatus::temporarily_unavailable
                                                    : current.disk_read_bytes.status);
            sample.disk_write_rate = unavailable<BytesPerSecond>(
                current.disk_write_bytes.has_value() ? MetricStatus::temporarily_unavailable
                                                     : current.disk_write_bytes.status);
        } else {
            const auto elapsed = raw.observed_at - previous->second.observed_at;
            sample.cpu_usage = normalize_process_cpu(
                previous->second.counters.cpu_time, current.cpu_time, elapsed,
                raw.system.logical_processor_count);
            sample.disk_read_rate = normalize_byte_rate(
                previous->second.counters.disk_read_bytes,
                current.disk_read_bytes, elapsed);
            sample.disk_write_rate = normalize_byte_rate(
                previous->second.counters.disk_write_bytes,
                current.disk_write_bytes, elapsed);
        }

        auto& stored = previous_[current.identity];
        stored = PreviousObservation{raw.observed_at, current, generation_};
        destination.push_back(std::move(sample));
    }

    for (auto iterator = previous_.begin(); iterator != previous_.end();) {
        if (iterator->second.generation != generation_) {
            iterator = previous_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void ProcessTelemetryNormalizer::reset() noexcept {
    previous_.clear();
    generation_ = 0U;
}

std::size_t ProcessTelemetryNormalizer::tracked_processes() const noexcept {
    return previous_.size();
}

} // namespace blackbox::telemetry
