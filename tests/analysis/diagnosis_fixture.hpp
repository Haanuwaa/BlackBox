#pragma once

#include "analysis/incident_analyzer.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace blackbox::test::diagnosis_fixture {

using namespace std::chrono_literals;

[[nodiscard]] inline std::shared_ptr<const core::IncidentSnapshot> incident(
    const std::optional<analysis::ResourceKind> primary,
    const std::optional<analysis::ResourceKind> secondary = std::nullopt,
    const bool automatic = false,
    const std::size_t process_count = 1U) {
    constexpr std::size_t frames = 150U;
    core::IncidentHeader header{};
    header.window.event_time = core::MonotonicTimePoint{120s};
    header.window.requested_start = core::MonotonicTimePoint{0s};
    header.window.requested_end = core::MonotonicTimePoint{149s};
    header.actual_start = header.window.requested_start;
    header.actual_end = header.window.requested_end;
    if (automatic && primary) {
        header.window.automatic_trigger_count = 1U;
        header.window.manual_trigger_count = 0U;
        header.window.automatic_score = 0.92;
        switch (*primary) {
        case analysis::ResourceKind::cpu:
            header.window.automatic_resource = core::AutomaticIncidentResource::cpu;
            break;
        case analysis::ResourceKind::memory:
            header.window.automatic_resource = core::AutomaticIncidentResource::memory;
            break;
        case analysis::ResourceKind::disk:
            header.window.automatic_resource = core::AutomaticIncidentResource::disk;
            break;
        case analysis::ResourceKind::network:
            header.window.automatic_resource = core::AutomaticIncidentResource::network;
            break;
        }
    }

    std::vector<core::IncidentProcessInfo> metadata;
    metadata.reserve(process_count);
    for (std::size_t index = 0U; index < process_count; ++index) {
        core::IncidentProcessInfo process{};
        process.identity = {static_cast<std::uint32_t>(100U + index),
                            static_cast<std::uint64_t>(1'000U + index)};
        process.name = {index == 0U ? "culprit.exe"
                                   : "background-" + std::to_string(index) + ".exe",
                        core::RecordedValueStatus::available};
        metadata.push_back(std::move(process));
    }

    const auto selected = [primary, secondary](const analysis::ResourceKind resource) {
        return primary == resource || secondary == resource;
    };
    constexpr double mebibyte = 1024.0 * 1024.0;
    std::vector<core::IncidentSystemSample> systems;
    std::vector<core::IncidentProcessSample> processes;
    systems.reserve(frames);
    processes.reserve(frames * process_count);
    for (std::size_t frame = 0U; frame < frames; ++frame) {
        const auto at = core::MonotonicTimePoint{
            std::chrono::seconds{static_cast<std::int64_t>(frame)}};
        const auto wave = static_cast<double>(static_cast<int>(frame % 5U) - 2);
        const auto event = frame >= 112U && frame <= 121U;
        core::IncidentSystemSample system{};
        system.observed_at = at;
        system.cpu_fraction = {
            event && selected(analysis::ResourceKind::cpu)
                ? 0.95 : 0.20 + wave * 0.002,
            core::RecordedValueStatus::available};
        system.memory_fraction = {
            event && selected(analysis::ResourceKind::memory)
                ? 0.92 : 0.50 + wave * 0.001,
            core::RecordedValueStatus::available};
        system.disk_read_bytes_per_second = {
            event && selected(analysis::ResourceKind::disk)
                ? 200.0 * mebibyte : 1.0 * mebibyte + wave * 4'096.0,
            core::RecordedValueStatus::available};
        system.disk_write_bytes_per_second = {
            0.5 * mebibyte, core::RecordedValueStatus::available};
        system.network_receive_bytes_per_second = {
            event && selected(analysis::ResourceKind::network)
                ? 150.0 * mebibyte : 0.25 * mebibyte + wave * 2'048.0,
            core::RecordedValueStatus::available};
        system.network_transmit_bytes_per_second = {
            0.125 * mebibyte, core::RecordedValueStatus::available};
        systems.push_back(system);

        for (std::size_t index = 0U; index < process_count; ++index) {
            core::IncidentProcessSample process{};
            process.observed_at = at;
            process.identity = metadata[index].identity;
            const auto culprit_event = index == 0U && event;
            process.cpu_fraction = {
                culprit_event && selected(analysis::ResourceKind::cpu)
                    ? 0.80 : 0.01 + wave * 0.0001,
                core::RecordedValueStatus::available};
            process.working_set_bytes = {
                culprit_event && selected(analysis::ResourceKind::memory)
                    ? static_cast<std::uint64_t>(900U) << 20U
                    : static_cast<std::uint64_t>(64U + index) << 20U,
                core::RecordedValueStatus::available};
            process.disk_read_bytes_per_second = {
                culprit_event && selected(analysis::ResourceKind::disk)
                    ? 150.0 * mebibyte : 64'000.0 + wave * 100.0,
                core::RecordedValueStatus::available};
            process.disk_write_bytes_per_second = {
                32'000.0, core::RecordedValueStatus::available};
            processes.push_back(process);
        }
    }
    return std::make_shared<const core::IncidentSnapshot>(
        std::move(header), std::move(systems), std::move(metadata),
        std::move(processes));
}

[[nodiscard]] constexpr analysis::IncidentType expected_type(
    const analysis::ResourceKind resource) noexcept {
    switch (resource) {
    case analysis::ResourceKind::cpu: return analysis::IncidentType::cpu_pressure;
    case analysis::ResourceKind::memory: return analysis::IncidentType::memory_pressure;
    case analysis::ResourceKind::disk: return analysis::IncidentType::storage_pressure;
    case analysis::ResourceKind::network: return analysis::IncidentType::network_pressure;
    }
    return analysis::IncidentType::unknown;
}

} // namespace blackbox::test::diagnosis_fixture
