#include "analysis/statistical_incident_analyzer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace analysis = blackbox::analysis;
namespace core = blackbox::core;
using namespace std::chrono_literals;

namespace {

enum class Spike {
    none,
    cpu,
    statistical_only_cpu,
    memory,
    disk,
    network,
    disk_quality,
    network_quality,
    process
};

[[nodiscard]] std::shared_ptr<const core::IncidentSnapshot> incident(
    const Spike spike = Spike::none, const bool all_system_missing = false,
    const std::size_t frame_count = 150U) {
    core::IncidentHeader header{};
    header.window.event_time = core::MonotonicTimePoint{120s};
    header.window.requested_start = core::MonotonicTimePoint{0s};
    header.window.requested_end = core::MonotonicTimePoint{150s};
    header.actual_start = core::MonotonicTimePoint{0s};
    header.actual_end = core::MonotonicTimePoint{
        std::chrono::seconds{static_cast<std::int64_t>(frame_count - 1U)}};

    const core::IncidentProcessIdentity first{10U, 100U};
    const core::IncidentProcessIdentity second{20U, 200U};
    std::vector<core::IncidentProcessInfo> metadata(2U);
    metadata[0].identity = first;
    metadata[0].name = {"normal.exe", core::RecordedValueStatus::available};
    metadata[1].identity = second;
    metadata[1].name = {"spike.exe", core::RecordedValueStatus::available};
    std::vector<core::IncidentSystemSample> systems;
    std::vector<core::IncidentProcessSample> processes;
    systems.reserve(frame_count);
    processes.reserve(frame_count * 2U);
    for (std::size_t frame = 0U; frame < frame_count; ++frame) {
        const auto at = core::MonotonicTimePoint{
            std::chrono::seconds{static_cast<std::int64_t>(frame)}};
        const auto wave = static_cast<double>(static_cast<int>(frame % 5U) - 2);
        core::IncidentSystemSample system{};
        system.observed_at = at;
        system.cpu_fraction = {0.20 + wave * 0.002,
                               core::RecordedValueStatus::available};
        system.memory_fraction = {0.50 + wave * 0.001,
                                  core::RecordedValueStatus::available};
        system.disk_read_bytes_per_second = {1'048'576.0 + wave * 4'096.0,
                                              core::RecordedValueStatus::available};
        system.disk_write_bytes_per_second = {524'288.0,
                                               core::RecordedValueStatus::available};
        system.network_receive_bytes_per_second = {262'144.0 + wave * 2'048.0,
                                                    core::RecordedValueStatus::available};
        system.network_transmit_bytes_per_second = {131'072.0,
                                                     core::RecordedValueStatus::available};
        system.disk_read_latency_seconds = {0.003 + wave * 0.00001,
                                            core::RecordedValueStatus::available};
        system.disk_write_latency_seconds = {0.004,
                                             core::RecordedValueStatus::available};
        system.disk_service_time_seconds = {0.004 + wave * 0.00001,
                                             core::RecordedValueStatus::available};
        system.disk_queue_depth = {0.2, core::RecordedValueStatus::available};
        system.network_connectivity_level = {2U,
                                              core::RecordedValueStatus::available};
        system.network_interface_changes = {0U,
                                             core::RecordedValueStatus::available};
        system.network_tcp_retransmit_fraction = {
            0.001, core::RecordedValueStatus::available};
        system.network_tcp_failed_connections = {
            0U, core::RecordedValueStatus::available};
        system.network_tcp_resets = {0U, core::RecordedValueStatus::available};
        if (all_system_missing) {
            system.cpu_fraction.status = core::RecordedValueStatus::unsupported;
            system.memory_fraction.status = core::RecordedValueStatus::inaccessible;
            system.disk_read_bytes_per_second.status =
                core::RecordedValueStatus::temporarily_unavailable;
            system.disk_write_bytes_per_second.status = core::RecordedValueStatus::unsupported;
            system.network_receive_bytes_per_second.status = core::RecordedValueStatus::unsupported;
            system.network_transmit_bytes_per_second.status = core::RecordedValueStatus::unsupported;
            system.disk_read_latency_seconds.status = core::RecordedValueStatus::unsupported;
            system.disk_write_latency_seconds.status = core::RecordedValueStatus::unsupported;
            system.disk_service_time_seconds.status = core::RecordedValueStatus::unsupported;
            system.disk_queue_depth.status = core::RecordedValueStatus::unsupported;
            system.network_connectivity_level.status = core::RecordedValueStatus::unsupported;
            system.network_interface_changes.status = core::RecordedValueStatus::unsupported;
            system.network_tcp_retransmit_fraction.status = core::RecordedValueStatus::unsupported;
            system.network_tcp_failed_connections.status = core::RecordedValueStatus::unsupported;
            system.network_tcp_resets.status = core::RecordedValueStatus::unsupported;
        } else if (frame == 120U) {
            if (spike == Spike::cpu) system.cpu_fraction.value = 0.95;
            if (spike == Spike::statistical_only_cpu)
                system.cpu_fraction.value = 0.40;
            if (spike == Spike::memory) system.memory_fraction.value = 0.90;
            if (spike == Spike::disk)
                system.disk_read_bytes_per_second.value = 200.0 * 1024.0 * 1024.0;
            if (spike == Spike::network)
                system.network_receive_bytes_per_second.value = 100.0 * 1024.0 * 1024.0;
            if (spike == Spike::disk_quality) {
                system.disk_service_time_seconds.value = 0.250;
                system.disk_queue_depth.value = 12.0;
            }
            if (spike == Spike::network_quality) {
                system.network_connectivity_level.value = 0U;
                system.network_interface_changes.value = 2U;
                system.network_tcp_retransmit_fraction.value = 0.40;
                system.network_tcp_failed_connections.value = 3U;
            }
        }
        systems.push_back(system);

        for (const auto identity : {first, second}) {
            core::IncidentProcessSample process{};
            process.observed_at = at;
            process.identity = identity;
            process.cpu_fraction = {(identity == first ? 0.01 : 0.02) + wave * 0.0001,
                                    core::RecordedValueStatus::available};
            process.working_set_bytes = {identity == first ? 64U << 20U : 96U << 20U,
                                         core::RecordedValueStatus::available};
            process.disk_read_bytes_per_second = {64'000.0,
                                                   core::RecordedValueStatus::available};
            process.disk_write_bytes_per_second = {32'000.0,
                                                    core::RecordedValueStatus::available};
            if (frame == 120U && spike == Spike::process && identity == second)
                process.cpu_fraction.value = 0.80;
            processes.push_back(process);
        }
    }
    return std::make_shared<const core::IncidentSnapshot>(
        std::move(header), std::move(systems), std::move(metadata),
        std::move(processes));
}

[[nodiscard]] const analysis::ResourceAnomaly& resource(
    const analysis::IncidentAnalysis& result, const analysis::ResourceKind kind) {
    const auto found = std::find_if(result.resources.begin(), result.resources.end(),
                                    [kind](const auto& value) {
                                        return value.resource == kind;
                                    });
    REQUIRE(found != result.resources.end());
    return *found;
}

} // namespace

TEST_CASE("statistical configuration rejects unbounded or unusable profiles",
          "[analysis][configuration]") {
    CHECK(analysis::validate_statistical_analysis_configuration({}).has_value());
    auto invalid = analysis::StatisticalAnalysisConfiguration{};
    invalid.minimum_baseline_samples = invalid.baseline_capacity + 1U;
    CHECK_FALSE(analysis::validate_statistical_analysis_configuration(invalid).has_value());
    invalid = {};
    invalid.maximum_ranked_processes = invalid.maximum_process_candidates + 1U;
    CHECK_FALSE(analysis::validate_statistical_analysis_configuration(invalid).has_value());
    invalid = {};
    invalid.resource_effect_floors.cpu_minimum_value =
        std::numeric_limits<double>::quiet_NaN();
    const auto invalid_floor = analysis::validate_statistical_analysis_configuration(invalid);
    REQUIRE_FALSE(invalid_floor.has_value());
    CHECK(invalid_floor.error() ==
          analysis::StatisticalAnalysisConfigurationError::resource_effect_floor_invalid);
}

TEST_CASE("fixed incidents produce deterministic rankings and normal fixtures stay quiet",
          "[analysis][determinism][false-positive]") {
    analysis::StatisticalIncidentAnalyzer analyzer;
    const auto fixture = incident();
    const auto first = analyzer.analyze(*fixture);
    const auto second = analyzer.analyze(*fixture);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == *second);
    CHECK_FALSE(first->cold_start);
    REQUIRE(first->resources.size() == 4U);
    CHECK(first->resources.front().score == 0.0);
    REQUIRE_FALSE(first->processes.empty());
    CHECK(first->processes.front().score == 0.0);
}

TEST_CASE("large statistical deviations below practical floors remain inspectable but abstain",
          "[analysis][resource][effect-floor][false-positive]") {
    analysis::StatisticalIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(*incident(Spike::statistical_only_cpu));
    REQUIRE(result.has_value());
    const auto& cpu = resource(*result, analysis::ResourceKind::cpu);
    CHECK(cpu.statistical_score > 0.99);
    CHECK(cpu.uncontextualized_score == 0.0);
    CHECK(cpu.score == 0.0);
    CHECK_FALSE(cpu.pressure_metric.has_value());
}

TEST_CASE("synthetic system spikes rank the injected resource first",
          "[analysis][resource][spike]") {
    analysis::StatisticalIncidentAnalyzer analyzer;
    const auto verify = [&](const Spike spike, const analysis::ResourceKind expected) {
        const auto result = analyzer.analyze(*incident(spike));
        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->resources.empty());
        CHECK(result->resources.front().resource == expected);
        CHECK(result->resources.front().score > 0.99);
        CHECK(result->resources.front().pressure_metric.has_value());
        CHECK(result->resources.front().confidence == analysis::AnalysisConfidence::high);
    };
    verify(Spike::cpu, analysis::ResourceKind::cpu);
    verify(Spike::memory, analysis::ResourceKind::memory);
    verify(Spike::disk, analysis::ResourceKind::disk);
    verify(Spike::network, analysis::ResourceKind::network);
    verify(Spike::disk_quality, analysis::ResourceKind::disk);
    verify(Spike::network_quality, analysis::ResourceKind::network);
}

TEST_CASE("synthetic process spike ranks the injected full identity first",
          "[analysis][process][spike]") {
    analysis::StatisticalIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(*incident(Spike::process));
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->processes.empty());
    CHECK(result->processes.front().identity == core::IncidentProcessIdentity{20U, 200U});
    CHECK(result->processes.front().name == "spike.exe");
    CHECK(result->processes.front().score > 0.99);
}

TEST_CASE("missing metrics and short baselines produce explicit cold-start uncertainty",
          "[analysis][missing][cold-start]") {
    analysis::StatisticalIncidentAnalyzer analyzer;
    const auto missing = analyzer.analyze(*incident(Spike::none, true));
    REQUIRE(missing.has_value());
    CHECK(missing->cold_start);
    CHECK(missing->missing_values > 0U);
    for (const auto& ranked : missing->resources) {
        CHECK(ranked.score == 0.0);
        CHECK(ranked.confidence == analysis::AnalysisConfidence::low);
    }

    const auto short_incident = incident(Spike::cpu, false, 20U);
    const auto short_result = analyzer.analyze(*short_incident);
    REQUIRE(short_result.has_value());
    CHECK(short_result->cold_start);
    CHECK(resource(*short_result, analysis::ResourceKind::cpu).score == 0.0);
}
