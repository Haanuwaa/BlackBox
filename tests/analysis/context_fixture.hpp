#pragma once

#include "analysis/workload_context.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace blackbox::test::context_fixture {

using namespace std::chrono_literals;

inline constexpr std::array labeled_contexts{
    analysis::WorkloadContextKind::idle,
    analysis::WorkloadContextKind::gaming,
    analysis::WorkloadContextKind::development,
    analysis::WorkloadContextKind::compilation,
    analysis::WorkloadContextKind::video_playback_or_call,
    analysis::WorkloadContextKind::heavy_download,
    analysis::WorkloadContextKind::desktop,
    analysis::WorkloadContextKind::unknown,
};

struct Shape final {
    double cpu{};
    double memory{};
    double disk_mib_per_second{};
    double network_mib_per_second{};
    const char* first_name{};
    const char* second_name{};
};

[[nodiscard]] inline Shape shape(const analysis::WorkloadContextKind context,
                                 const std::size_t variant) noexcept {
    switch (context) {
    case analysis::WorkloadContextKind::idle:
        return {0.01, 0.20, 0.02, 0.01,
                variant == 0U ? "background.exe" : "service-host.exe", ""};
    case analysis::WorkloadContextKind::gaming:
        return {0.72, 0.66, 2.0, 1.0,
                variant == 0U ? "gameclient.exe" : "unreal-demo.exe", ""};
    case analysis::WorkloadContextKind::development:
        return {0.24, 0.61, 3.0, 0.2,
                variant == 0U ? "devenv.exe" : "pycharm64.exe", ""};
    case analysis::WorkloadContextKind::compilation:
        return {0.86, 0.56, 54.0, 0.2,
                variant == 0U ? "ninja.exe" : "rustc.exe", ""};
    case analysis::WorkloadContextKind::video_playback_or_call:
        return {0.31, 0.46, 2.0, 12.0,
                variant == 0U ? "zoom.exe" : "vlc.exe", ""};
    case analysis::WorkloadContextKind::heavy_download:
        return {0.16, 0.41, 58.0, 110.0,
                variant == 0U ? "aria2c.exe" : "wget.exe", ""};
    case analysis::WorkloadContextKind::desktop:
        return {0.09, 0.35, 0.3, 0.1,
                variant == 0U ? "explorer.exe" : "dwm.exe", ""};
    case analysis::WorkloadContextKind::unknown:
        return {0.39, 0.50, 11.0, 11.0,
                variant == 0U ? "workload.exe" : "custom-task.exe", ""};
    }
    return {};
}

[[nodiscard]] inline std::shared_ptr<const core::IncidentSnapshot> incident(
    const analysis::WorkloadContextKind context, const std::size_t variant = 0U,
    const std::size_t frames = 150U) {
    core::IncidentHeader header{};
    header.window.event_time = core::MonotonicTimePoint{120s};
    header.window.requested_start = core::MonotonicTimePoint{0s};
    header.window.requested_end = core::MonotonicTimePoint{150s};
    header.actual_start = core::MonotonicTimePoint{0s};
    header.actual_end = core::MonotonicTimePoint{
        std::chrono::seconds{static_cast<std::int64_t>(frames - 1U)}};

    const auto workload = shape(context, variant);
    std::vector<core::IncidentSystemSample> systems;
    systems.reserve(frames);
    constexpr double mebibyte = 1024.0 * 1024.0;
    for (std::size_t frame = 0U; frame < frames; ++frame) {
        const auto wave = static_cast<double>(static_cast<int>(frame % 5U) - 2);
        core::IncidentSystemSample sample{};
        sample.observed_at = core::MonotonicTimePoint{
            std::chrono::seconds{static_cast<std::int64_t>(frame)}};
        sample.cpu_fraction = {std::clamp(workload.cpu + wave * 0.001, 0.0, 1.0),
                               core::RecordedValueStatus::available};
        sample.memory_fraction = {
            std::clamp(workload.memory + wave * 0.0005, 0.0, 1.0),
            core::RecordedValueStatus::available};
        sample.disk_read_bytes_per_second = {
            workload.disk_mib_per_second * mebibyte,
            core::RecordedValueStatus::available};
        sample.disk_write_bytes_per_second = {
            workload.disk_mib_per_second * mebibyte * 0.25,
            core::RecordedValueStatus::available};
        sample.network_receive_bytes_per_second = {
            workload.network_mib_per_second * mebibyte,
            core::RecordedValueStatus::available};
        sample.network_transmit_bytes_per_second = {
            workload.network_mib_per_second * mebibyte * 0.10,
            core::RecordedValueStatus::available};
        systems.push_back(sample);
    }

    std::vector<core::IncidentProcessInfo> metadata;
    if (workload.first_name[0] != '\0') {
        core::IncidentProcessInfo process{};
        process.identity = {10U, 100U};
        process.name = {std::string{workload.first_name},
                        core::RecordedValueStatus::available};
        metadata.push_back(std::move(process));
    }
    if (workload.second_name[0] != '\0') {
        core::IncidentProcessInfo process{};
        process.identity = {20U, 200U};
        process.name = {std::string{workload.second_name},
                        core::RecordedValueStatus::available};
        metadata.push_back(std::move(process));
    }
    return std::make_shared<const core::IncidentSnapshot>(
        std::move(header), std::move(systems), std::move(metadata),
        std::vector<core::IncidentProcessSample>{});
}

[[nodiscard]] inline analysis::ResourceKind expected_resource(
    const analysis::WorkloadContextKind context) noexcept {
    switch (context) {
    case analysis::WorkloadContextKind::gaming:
    case analysis::WorkloadContextKind::compilation: return analysis::ResourceKind::cpu;
    case analysis::WorkloadContextKind::development:
    case analysis::WorkloadContextKind::desktop: return analysis::ResourceKind::memory;
    case analysis::WorkloadContextKind::video_playback_or_call:
    case analysis::WorkloadContextKind::heavy_download: return analysis::ResourceKind::network;
    case analysis::WorkloadContextKind::idle:
    case analysis::WorkloadContextKind::unknown: return analysis::ResourceKind::cpu;
    }
    return analysis::ResourceKind::cpu;
}

[[nodiscard]] inline analysis::ResourceKind held_out_target(
    const analysis::WorkloadContextKind context) noexcept {
    switch (context) {
    case analysis::WorkloadContextKind::gaming: return analysis::ResourceKind::disk;
    case analysis::WorkloadContextKind::development: return analysis::ResourceKind::network;
    case analysis::WorkloadContextKind::compilation: return analysis::ResourceKind::network;
    case analysis::WorkloadContextKind::video_playback_or_call: return analysis::ResourceKind::disk;
    case analysis::WorkloadContextKind::heavy_download: return analysis::ResourceKind::cpu;
    case analysis::WorkloadContextKind::desktop: return analysis::ResourceKind::network;
    case analysis::WorkloadContextKind::idle:
    case analysis::WorkloadContextKind::unknown: return analysis::ResourceKind::memory;
    }
    return analysis::ResourceKind::memory;
}

[[nodiscard]] inline analysis::IncidentAnalysis ranking_fixture(
    analysis::WorkloadContextAssessment assessment,
    const analysis::WorkloadContextKind context,
    const bool baseline_already_correct = false) {
    analysis::IncidentAnalysis result{};
    result.workload_context = std::move(assessment);
    const auto expected = expected_resource(context);
    const auto target = held_out_target(context);
    for (const auto resource : {analysis::ResourceKind::cpu,
                                analysis::ResourceKind::memory,
                                analysis::ResourceKind::disk,
                                analysis::ResourceKind::network}) {
        analysis::ResourceAnomaly anomaly{};
        anomaly.resource = resource;
        anomaly.uncontextualized_score = resource == target
                                             ? (baseline_already_correct ? 0.92 : 0.90)
                                             : resource == expected
                                                   ? (baseline_already_correct ? 0.50 : 0.92)
                                                   : 0.20;
        anomaly.score = anomaly.uncontextualized_score;
        result.resources.push_back(anomaly);
    }
    std::sort(result.resources.begin(), result.resources.end(),
              [](const auto& left, const auto& right) {
                  if (left.score != right.score) return left.score > right.score;
                  return left.resource < right.resource;
              });
    return result;
}

} // namespace blackbox::test::context_fixture
