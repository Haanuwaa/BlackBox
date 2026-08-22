#pragma once

#include "telemetry/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace blackbox::telemetry {

class ProcessMetadataCache final {
public:
    explicit ProcessMetadataCache(
        std::chrono::nanoseconds retention,
        std::size_t maximum_entries = 8'192U);

    void update(std::span<const ProcessInfo> metadata,
                std::span<const ProcessSample> active_processes,
                core::MonotonicTimePoint observed_at);
    void reset(std::chrono::nanoseconds retention);

    [[nodiscard]] std::vector<ProcessInfo> snapshot() const;
    [[nodiscard]] std::vector<ProcessInfo> active_snapshot(
        std::span<const ProcessSample> active_processes) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t maximum_entries() const noexcept;
    [[nodiscard]] std::uint64_t evictions() const noexcept;

private:
    struct IdentityHash {
        [[nodiscard]] std::size_t operator()(const ProcessIdentity& identity) const noexcept;
    };
    struct Entry {
        ProcessInfo info{};
        core::MonotonicTimePoint last_seen{};
        std::uint64_t generation{};
    };

    [[nodiscard]] bool make_room();

    std::chrono::nanoseconds retention_{};
    std::size_t maximum_entries_{};
    std::unordered_map<ProcessIdentity, Entry, IdentityHash> entries_{};
    std::uint64_t generation_{};
    std::uint64_t evictions_{};
};

} // namespace blackbox::telemetry
