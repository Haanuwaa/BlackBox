#pragma once

#include "core/incident.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace blackbox::analysis {

inline constexpr std::int32_t incident_feature_version = 2;
inline constexpr std::size_t incident_feature_dimension_count = 16U;
inline constexpr std::size_t maximum_clustered_incidents = 512U;
inline constexpr std::size_t maximum_cluster_characteristics = 3U;
inline constexpr double incident_cluster_distance_threshold = 0.20;

enum class IncidentFeatureDimension : std::uint8_t {
    cpu_peak,
    cpu_near_marker,
    memory_peak,
    memory_near_marker,
    disk_peak,
    disk_near_marker,
    network_peak,
    network_near_marker,
    dominant_pre_marker_share,
    dominant_post_marker_share,
    duration,
    dominant_resource_concentration,
    disk_quality_peak,
    disk_quality_near_marker,
    network_quality_peak,
    network_quality_near_marker,
};

struct IncidentFeatureVector {
    std::int64_t incident_id{};
    std::int64_t created_utc_milliseconds{};
    std::int32_t version{incident_feature_version};
    std::array<double, incident_feature_dimension_count> values{};
    std::array<bool, incident_feature_dimension_count> available{};
    friend bool operator==(const IncidentFeatureVector&,
                           const IncidentFeatureVector&) = default;
};

struct IncidentClusterInput {
    IncidentFeatureVector feature{};
    std::string override_group{};
    friend bool operator==(const IncidentClusterInput&,
                           const IncidentClusterInput&) = default;
};

struct IncidentClusterCharacteristic {
    IncidentFeatureDimension dimension{IncidentFeatureDimension::cpu_peak};
    double median{};
    double support{};
    friend bool operator==(const IncidentClusterCharacteristic&,
                           const IncidentClusterCharacteristic&) = default;
};

struct IncidentCluster {
    std::int64_t stable_key{};
    bool manually_overridden{};
    std::string override_group{};
    std::vector<std::int64_t> incident_ids{};
    std::vector<IncidentClusterCharacteristic> shared_characteristics{};
    double maximum_pair_distance{};
    friend bool operator==(const IncidentCluster&, const IncidentCluster&) = default;
};

struct IncidentClusteringResult {
    std::int32_t feature_version{incident_feature_version};
    std::size_t inputs_considered{};
    std::vector<IncidentCluster> clusters{};
    std::vector<std::int64_t> noise_incident_ids{};
    friend bool operator==(const IncidentClusteringResult&,
                           const IncidentClusteringResult&) = default;
};

[[nodiscard]] IncidentFeatureVector extract_incident_features(
    std::int64_t incident_id, std::int64_t created_utc_milliseconds,
    const core::IncidentSnapshot& incident) noexcept;

[[nodiscard]] double incident_feature_distance(
    const IncidentFeatureVector& left,
    const IncidentFeatureVector& right) noexcept;

// Deterministic, local, bounded complete-link threshold grouping. Override labels are explicit
// user decisions and group matching labels regardless of statistical distance.
[[nodiscard]] IncidentClusteringResult cluster_incidents(
    std::span<const IncidentClusterInput> inputs);

} // namespace blackbox::analysis
