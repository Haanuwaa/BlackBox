#include "analysis/incident_clustering.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace analysis = blackbox::analysis;
namespace core = blackbox::core;
using namespace std::chrono_literals;

namespace {

enum class Shape { cpu, disk, balanced };

[[nodiscard]] core::IncidentSnapshot incident(const Shape shape,
                                              const double variation = 0.0) {
    core::IncidentHeader header{};
    header.window.event_time = core::MonotonicTimePoint{100s};
    header.window.requested_start = core::MonotonicTimePoint{0s};
    header.window.requested_end = core::MonotonicTimePoint{130s};
    header.actual_start = header.window.requested_start;
    header.actual_end = header.window.requested_end;
    std::vector<core::IncidentSystemSample> samples;
    samples.reserve(131U);
    for (std::int64_t second = 0; second <= 130; ++second) {
        const auto near = second >= 92 && second <= 108;
        core::IncidentSystemSample sample{};
        sample.observed_at = core::MonotonicTimePoint{std::chrono::seconds{second}};
        const auto cpu_peak = shape == Shape::cpu ? 0.90 + variation
                             : shape == Shape::balanced ? 0.55 + variation
                                                        : 0.18 + variation;
        sample.cpu_fraction = {near ? cpu_peak : 0.15,
                               core::RecordedValueStatus::available};
        sample.memory_fraction = {shape == Shape::balanced && near ? 0.75 : 0.42,
                                  core::RecordedValueStatus::available};
        const auto disk_peak = shape == Shape::disk ? 700.0 * 1024.0 * 1024.0
                               : shape == Shape::balanced
                                   ? 90.0 * 1024.0 * 1024.0
                                   : 1.0 * 1024.0 * 1024.0;
        sample.disk_read_bytes_per_second = {
            near ? disk_peak * (1.0 + variation) : 256.0 * 1024.0,
            core::RecordedValueStatus::available};
        sample.disk_write_bytes_per_second = {0.0,
                                               core::RecordedValueStatus::available};
        sample.network_receive_bytes_per_second = {128.0 * 1024.0,
                                                    core::RecordedValueStatus::available};
        sample.network_transmit_bytes_per_second = {64.0 * 1024.0,
                                                     core::RecordedValueStatus::available};
        samples.push_back(sample);
    }
    return core::IncidentSnapshot{std::move(header), std::move(samples), {}, {}};
}

[[nodiscard]] analysis::IncidentClusterInput input(
    const std::int64_t id, const std::int64_t date, const Shape shape,
    const double variation = 0.0, std::string override_group = {}) {
    return {analysis::extract_incident_features(id, date,
                                                incident(shape, variation)),
            std::move(override_group)};
}

[[nodiscard]] const analysis::IncidentCluster* cluster_containing(
    const analysis::IncidentClusteringResult& result, const std::int64_t id) {
    const auto found = std::find_if(result.clusters.begin(), result.clusters.end(),
                                    [id](const auto& cluster) {
        return std::find(cluster.incident_ids.begin(), cluster.incident_ids.end(), id) !=
               cluster.incident_ids.end();
    });
    return found == result.clusters.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("versioned incident features use bounded explicit scaling",
          "[analysis][cluster][feature]") {
    const auto cpu = analysis::extract_incident_features(1, 1'000,
                                                          incident(Shape::cpu));
    CHECK(cpu.version == analysis::incident_feature_version);
    CHECK(cpu.values.size() == analysis::incident_feature_dimension_count);
    CHECK(cpu.available[0]);
    CHECK(cpu.values[0] > 0.89);
    CHECK(cpu.values[4] >= 0.0);
    for (std::size_t index = 0U; index < cpu.values.size(); ++index) {
        if (cpu.available[index]) {
            CHECK(cpu.values[index] >= 0.0);
            CHECK(cpu.values[index] <= 1.0);
        }
    }
}

TEST_CASE("similar recurring shapes group across dates while resources remain separate",
          "[analysis][cluster][quality]") {
    std::vector<analysis::IncidentClusterInput> inputs{
        input(1, 1'000, Shape::cpu, -0.01), input(2, 2'000, Shape::cpu),
        input(3, 3'000, Shape::cpu, 0.01), input(4, 4'000, Shape::disk, -0.01),
        input(5, 5'000, Shape::disk), input(6, 6'000, Shape::disk, 0.01),
        input(7, 7'000, Shape::balanced)};
    const auto result = analysis::cluster_incidents(inputs);
    REQUIRE(result.clusters.size() == 2U);
    REQUIRE(result.noise_incident_ids == std::vector<std::int64_t>{7});
    const auto* cpu = cluster_containing(result, 1);
    const auto* disk = cluster_containing(result, 4);
    REQUIRE(cpu != nullptr);
    REQUIRE(disk != nullptr);
    CHECK(cpu != disk);
    CHECK(cpu->incident_ids == std::vector<std::int64_t>{1, 2, 3});
    CHECK(disk->incident_ids == std::vector<std::int64_t>{4, 5, 6});
    CHECK_FALSE(cpu->shared_characteristics.empty());
    CHECK(analysis::incident_feature_distance(inputs[0].feature,
                                              inputs[3].feature) >
          analysis::incident_cluster_distance_threshold);
}

TEST_CASE("clustering is stable across input order and hard capped",
          "[analysis][cluster][stability][bounded]") {
    std::vector<analysis::IncidentClusterInput> inputs;
    inputs.reserve(520U);
    const auto prototype = analysis::extract_incident_features(
        1, 1, incident(Shape::cpu));
    for (std::int64_t id = 1; id <= 520; ++id) {
        auto feature = prototype;
        feature.incident_id = id;
        feature.created_utc_milliseconds = id;
        inputs.push_back({feature, {}});
    }
    const auto forward = analysis::cluster_incidents(inputs);
    std::reverse(inputs.begin(), inputs.end());
    const auto reversed = analysis::cluster_incidents(inputs);
    CHECK(forward == reversed);
    CHECK(forward.inputs_considered == analysis::maximum_clustered_incidents);
    REQUIRE(forward.clusters.size() == 1U);
    CHECK(forward.clusters.front().incident_ids.size() ==
          analysis::maximum_clustered_incidents);
    CHECK(forward.clusters.front().incident_ids.front() == 9);
}

TEST_CASE("manual labels override statistical grouping and remain inspectable",
          "[analysis][cluster][override]") {
    std::vector<analysis::IncidentClusterInput> inputs{
        input(1, 1'000, Shape::cpu, 0.0, "same symptom"),
        input(2, 2'000, Shape::disk, 0.0, "same symptom"),
        input(3, 3'000, Shape::balanced)};
    const auto result = analysis::cluster_incidents(inputs);
    REQUIRE(result.clusters.size() == 1U);
    const auto& manual = result.clusters.front();
    CHECK(manual.manually_overridden);
    CHECK(manual.override_group == "same symptom");
    CHECK(manual.incident_ids == std::vector<std::int64_t>{1, 2});
    CHECK(manual.maximum_pair_distance >
          analysis::incident_cluster_distance_threshold);
    CHECK(result.noise_incident_ids == std::vector<std::int64_t>{3});
}
