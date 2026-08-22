#include "analysis/incident_clustering.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <utility>

namespace blackbox::analysis {
namespace {

constexpr auto marker_radius = std::chrono::seconds{15};
constexpr double throughput_scale = 1024.0 * 1024.0 * 1024.0;

[[nodiscard]] double clamp_unit(const double value) noexcept {
    return std::clamp(std::isfinite(value) ? value : 0.0, 0.0, 1.0);
}

[[nodiscard]] double scale_throughput(const double value) noexcept {
    if (!std::isfinite(value) || value <= 0.0) return 0.0;
    return clamp_unit(std::log1p(value) / std::log1p(throughput_scale));
}

struct ResourceShape {
    double peak{};
    double marker_peak{};
    double pre_peak{};
    double post_peak{};
    bool available{};
    bool marker_available{};
    bool pre_available{};
    bool post_available{};
};

template <typename Reader>
void observe(ResourceShape& shape, const core::IncidentSystemSample& sample,
             const core::MonotonicTimePoint marker, Reader&& reader) noexcept {
    const auto value = reader(sample);
    if (!value.has_value()) return;
    shape.available = true;
    shape.peak = (std::max)(shape.peak, *value);
    if (sample.observed_at >= marker - marker_radius &&
        sample.observed_at <= marker + marker_radius) {
        shape.marker_available = true;
        shape.marker_peak = (std::max)(shape.marker_peak, *value);
    }
    if (sample.observed_at <= marker) {
        shape.pre_available = true;
        shape.pre_peak = (std::max)(shape.pre_peak, *value);
    } else {
        shape.post_available = true;
        shape.post_peak = (std::max)(shape.post_peak, *value);
    }
}

[[nodiscard]] std::optional<double> ratio(
    const core::RecordedValue<double>& value) noexcept {
    if (value.status != core::RecordedValueStatus::available ||
        !std::isfinite(value.value)) return std::nullopt;
    return clamp_unit(value.value);
}

[[nodiscard]] std::optional<double> combined_throughput(
    const core::RecordedValue<double>& first,
    const core::RecordedValue<double>& second) noexcept {
    double total{};
    bool available{};
    for (const auto* value : {&first, &second}) {
        if (value->status == core::RecordedValueStatus::available &&
            std::isfinite(value->value) && value->value >= 0.0) {
            total += value->value;
            available = true;
        }
    }
    return available ? std::optional{scale_throughput(total)} : std::nullopt;
}

[[nodiscard]] std::optional<double> disk_quality(
    const core::IncidentSystemSample& sample) noexcept {
    double result{};
    bool available{};
    for (const auto* value : {&sample.disk_read_latency_seconds,
                              &sample.disk_write_latency_seconds,
                              &sample.disk_service_time_seconds}) {
        if (value->status == core::RecordedValueStatus::available &&
            std::isfinite(value->value) && value->value >= 0.0) {
            result = (std::max)(result, clamp_unit(value->value / 0.200));
            available = true;
        }
    }
    if (sample.disk_queue_depth.status == core::RecordedValueStatus::available &&
        std::isfinite(sample.disk_queue_depth.value) &&
        sample.disk_queue_depth.value >= 0.0) {
        result = (std::max)(result,
                            clamp_unit(sample.disk_queue_depth.value / 16.0));
        available = true;
    }
    return available ? std::optional{result} : std::nullopt;
}

[[nodiscard]] std::optional<double> network_quality(
    const core::IncidentSystemSample& sample) noexcept {
    double result{};
    bool available{};
    if (sample.network_connectivity_level.status ==
        core::RecordedValueStatus::available) {
        const auto level = sample.network_connectivity_level.value;
        result = level == 0U ? 1.0 : level == 3U ? 0.75 : level == 1U ? 0.5 : 0.0;
        available = true;
    }
    if (sample.network_tcp_retransmit_fraction.status ==
            core::RecordedValueStatus::available &&
        std::isfinite(sample.network_tcp_retransmit_fraction.value) &&
        sample.network_tcp_retransmit_fraction.value >= 0.0) {
        result = (std::max)(result,
                            clamp_unit(sample.network_tcp_retransmit_fraction.value));
        available = true;
    }
    for (const auto* count : {&sample.network_interface_changes,
                              &sample.network_tcp_failed_connections,
                              &sample.network_tcp_resets}) {
        if (count->status == core::RecordedValueStatus::available) {
            result = (std::max)(result,
                                clamp_unit(static_cast<double>(count->value)));
            available = true;
        }
    }
    return available ? std::optional{result} : std::nullopt;
}

void assign_shape(IncidentFeatureVector& output, const std::size_t peak_index,
                  const ResourceShape& shape) noexcept {
    output.values[peak_index] = shape.peak;
    output.available[peak_index] = shape.available;
    output.values[peak_index + 1U] = shape.marker_peak;
    output.available[peak_index + 1U] = shape.marker_available;
}

[[nodiscard]] std::vector<IncidentClusterCharacteristic> characteristics(
    const std::vector<const IncidentClusterInput*>& members) {
    struct Ranked {
        IncidentClusterCharacteristic characteristic{};
        double rank{};
    };
    std::vector<Ranked> ranked;
    ranked.reserve(incident_feature_dimension_count);
    for (std::size_t dimension = 0U; dimension < incident_feature_dimension_count;
         ++dimension) {
        std::vector<double> values;
        values.reserve(members.size());
        for (const auto* member : members) {
            if (member->feature.available[dimension])
                values.push_back(member->feature.values[dimension]);
        }
        if (values.empty() || values.size() * 2U < members.size()) continue;
        std::sort(values.begin(), values.end());
        const auto median = values[values.size() / 2U];
        const auto support = static_cast<double>(values.size()) /
                             static_cast<double>(members.size());
        ranked.push_back({IncidentClusterCharacteristic{
                              static_cast<IncidentFeatureDimension>(dimension),
                              median, support},
                          median * support});
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        if (left.rank != right.rank) return left.rank > right.rank;
        return left.characteristic.dimension < right.characteristic.dimension;
    });
    if (ranked.size() > maximum_cluster_characteristics)
        ranked.resize(maximum_cluster_characteristics);
    std::vector<IncidentClusterCharacteristic> result;
    result.reserve(ranked.size());
    for (auto& item : ranked) result.push_back(std::move(item.characteristic));
    return result;
}

[[nodiscard]] IncidentCluster make_cluster(
    std::vector<const IncidentClusterInput*> members, const bool manual,
    std::string override_group) {
    std::sort(members.begin(), members.end(), [](const auto* left, const auto* right) {
        return left->feature.incident_id < right->feature.incident_id;
    });
    IncidentCluster result{};
    result.stable_key = members.front()->feature.incident_id;
    result.manually_overridden = manual;
    result.override_group = std::move(override_group);
    result.incident_ids.reserve(members.size());
    for (const auto* member : members)
        result.incident_ids.push_back(member->feature.incident_id);
    result.shared_characteristics = characteristics(members);
    for (std::size_t left = 0U; left < members.size(); ++left) {
        for (std::size_t right = left + 1U; right < members.size(); ++right) {
            result.maximum_pair_distance = (std::max)(
                result.maximum_pair_distance,
                incident_feature_distance(members[left]->feature,
                                          members[right]->feature));
        }
    }
    return result;
}

} // namespace

IncidentFeatureVector extract_incident_features(
    const std::int64_t incident_id, const std::int64_t created_utc_milliseconds,
    const core::IncidentSnapshot& incident) noexcept {
    IncidentFeatureVector result{};
    result.incident_id = incident_id;
    result.created_utc_milliseconds = created_utc_milliseconds;
    const auto marker = incident.header().window.event_time;
    std::array<ResourceShape, 4U> shapes{};
    ResourceShape disk_quality_shape{};
    ResourceShape network_quality_shape{};
    for (const auto& sample : incident.system_samples()) {
        observe(shapes[0], sample, marker,
                [](const auto& value) { return ratio(value.cpu_fraction); });
        observe(shapes[1], sample, marker,
                [](const auto& value) { return ratio(value.memory_fraction); });
        observe(shapes[2], sample, marker, [](const auto& value) {
            return combined_throughput(value.disk_read_bytes_per_second,
                                       value.disk_write_bytes_per_second);
        });
        observe(shapes[3], sample, marker, [](const auto& value) {
            return combined_throughput(value.network_receive_bytes_per_second,
                                       value.network_transmit_bytes_per_second);
        });
        observe(disk_quality_shape, sample, marker,
                [](const auto& value) { return disk_quality(value); });
        observe(network_quality_shape, sample, marker,
                [](const auto& value) { return network_quality(value); });
    }
    for (std::size_t resource = 0U; resource < shapes.size(); ++resource)
        assign_shape(result, resource * 2U, shapes[resource]);
    assign_shape(result, 12U, disk_quality_shape);
    assign_shape(result, 14U, network_quality_shape);

    const auto dominant = static_cast<std::size_t>(std::distance(
        shapes.begin(), std::max_element(shapes.begin(), shapes.end(),
            [](const auto& left, const auto& right) { return left.peak < right.peak; })));
    if (shapes[dominant].available) {
        const auto total = shapes[dominant].pre_peak + shapes[dominant].post_peak;
        if (total > 0.0 && shapes[dominant].pre_available &&
            shapes[dominant].post_available) {
            result.values[8] = shapes[dominant].pre_peak / total;
            result.values[9] = shapes[dominant].post_peak / total;
            result.available[8] = true;
            result.available[9] = true;
        }
        double resource_total{};
        for (const auto& shape : shapes)
            if (shape.available) resource_total += shape.peak;
        if (resource_total > 0.0) {
            result.values[11] = shapes[dominant].peak / resource_total;
            result.available[11] = true;
        }
    }
    const auto duration = std::chrono::duration<double>{
        incident.header().actual_end - incident.header().actual_start}.count();
    if (std::isfinite(duration) && duration >= 0.0) {
        result.values[10] = clamp_unit(duration / 150.0);
        result.available[10] = true;
    }
    return result;
}

double incident_feature_distance(const IncidentFeatureVector& left,
                                 const IncidentFeatureVector& right) noexcept {
    constexpr double missing_mismatch = 0.35;
    double squared{};
    std::size_t compared{};
    std::size_t shared{};
    for (std::size_t index = 0U; index < incident_feature_dimension_count; ++index) {
        if (left.available[index] && right.available[index]) {
            const auto difference = left.values[index] - right.values[index];
            squared += difference * difference;
            ++compared;
            ++shared;
        } else if (left.available[index] != right.available[index]) {
            squared += missing_mismatch * missing_mismatch;
            ++compared;
        }
    }
    if (shared < 4U || compared == 0U) return 1.0;
    return std::sqrt(squared / static_cast<double>(compared));
}

IncidentClusteringResult cluster_incidents(
    const std::span<const IncidentClusterInput> inputs) {
    IncidentClusteringResult result{};
    std::vector<const IncidentClusterInput*> bounded;
    bounded.reserve((std::min)(inputs.size(), maximum_clustered_incidents));
    for (const auto& input : inputs) {
        if (input.feature.version != incident_feature_version ||
            input.feature.incident_id <= 0) continue;
        bool valid = true;
        for (std::size_t index = 0U; index < incident_feature_dimension_count; ++index) {
            if (input.feature.available[index] &&
                (!std::isfinite(input.feature.values[index]) ||
                 input.feature.values[index] < 0.0 || input.feature.values[index] > 1.0)) {
                valid = false;
                break;
            }
        }
        if (valid) bounded.push_back(&input);
    }
    std::sort(bounded.begin(), bounded.end(), [](const auto* left, const auto* right) {
        if (left->feature.created_utc_milliseconds !=
            right->feature.created_utc_milliseconds)
            return left->feature.created_utc_milliseconds >
                   right->feature.created_utc_milliseconds;
        return left->feature.incident_id > right->feature.incident_id;
    });
    if (bounded.size() > maximum_clustered_incidents)
        bounded.resize(maximum_clustered_incidents);
    result.inputs_considered = bounded.size();

    std::map<std::string, std::vector<const IncidentClusterInput*>, std::less<>> manual;
    std::vector<const IncidentClusterInput*> automatic;
    automatic.reserve(bounded.size());
    for (const auto* input : bounded) {
        if (input->override_group.empty()) automatic.push_back(input);
        else manual[input->override_group].push_back(input);
    }
    for (auto& [label, members] : manual)
        result.clusters.push_back(make_cluster(std::move(members), true, label));

    std::sort(automatic.begin(), automatic.end(), [](const auto* left, const auto* right) {
        return left->feature.incident_id < right->feature.incident_id;
    });
    std::vector<std::vector<const IncidentClusterInput*>> components;
    for (const auto* input : automatic) {
        auto best = components.end();
        double best_maximum = std::numeric_limits<double>::infinity();
        for (auto candidate = components.begin(); candidate != components.end(); ++candidate) {
            double maximum{};
            bool compatible = true;
            for (const auto* member : *candidate) {
                maximum = (std::max)(maximum, incident_feature_distance(
                    input->feature, member->feature));
                if (maximum > incident_cluster_distance_threshold) {
                    compatible = false;
                    break;
                }
            }
            if (compatible && maximum < best_maximum) {
                best = candidate;
                best_maximum = maximum;
            }
        }
        if (best == components.end()) components.push_back({input});
        else best->push_back(input);
    }
    for (auto& members : components) {
        if (members.size() < 2U) {
            result.noise_incident_ids.push_back(members.front()->feature.incident_id);
        } else {
            result.clusters.push_back(make_cluster(std::move(members), false, {}));
        }
    }
    std::sort(result.clusters.begin(), result.clusters.end(), [](const auto& left,
                                                                 const auto& right) {
        if (left.manually_overridden != right.manually_overridden)
            return left.manually_overridden > right.manually_overridden;
        if (left.override_group != right.override_group)
            return left.override_group < right.override_group;
        return left.stable_key < right.stable_key;
    });
    std::sort(result.noise_incident_ids.begin(), result.noise_incident_ids.end());
    return result;
}

} // namespace blackbox::analysis
