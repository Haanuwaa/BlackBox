#include "telemetry/process_metadata_cache.hpp"

#include <algorithm>
#include <limits>

namespace blackbox::telemetry {

ProcessMetadataCache::ProcessMetadataCache(
    const std::chrono::nanoseconds retention,
    const std::size_t maximum_entries)
    : retention_{retention}, maximum_entries_{maximum_entries} {
    entries_.reserve(std::min<std::size_t>(maximum_entries_, 512U));
}

std::size_t ProcessMetadataCache::IdentityHash::operator()(
    const ProcessIdentity& identity) const noexcept {
    const auto pid = static_cast<std::uint64_t>(identity.pid.value);
    return static_cast<std::size_t>(identity.creation_token ^
                                    (pid * 0x9e3779b97f4a7c15ULL));
}

void ProcessMetadataCache::update(
    const std::span<const ProcessInfo> metadata,
    const std::span<const ProcessSample> active_processes,
    const core::MonotonicTimePoint observed_at) {
    ++generation_;
    for (const auto& process : active_processes) {
        const auto iterator = entries_.find(process.identity);
        if (iterator != entries_.end()) {
            iterator->second.last_seen = observed_at;
            iterator->second.generation = generation_;
        }
    }

    for (const auto& info : metadata) {
        auto iterator = entries_.find(info.identity);
        if (iterator == entries_.end()) {
            if (!make_room()) {
                continue;
            }
            iterator = entries_.emplace(info.identity, Entry{}).first;
        }
        iterator->second.info = info;
        iterator->second.last_seen = observed_at;
        iterator->second.generation = generation_;
    }

    for (auto iterator = entries_.begin(); iterator != entries_.end();) {
        const bool inactive = iterator->second.generation != generation_;
        const bool monotonic = observed_at >= iterator->second.last_seen;
        if (inactive && monotonic &&
            observed_at - iterator->second.last_seen > retention_) {
            iterator = entries_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

bool ProcessMetadataCache::make_room() {
    if (maximum_entries_ == 0U) {
        return false;
    }
    if (entries_.size() < maximum_entries_) {
        return true;
    }
    auto oldest = entries_.end();
    for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
        if (iterator->second.generation == generation_) {
            continue;
        }
        if (oldest == entries_.end() ||
            iterator->second.last_seen < oldest->second.last_seen) {
            oldest = iterator;
        }
    }
    if (oldest == entries_.end()) {
        return false;
    }
    entries_.erase(oldest);
    ++evictions_;
    return true;
}

void ProcessMetadataCache::reset(const std::chrono::nanoseconds retention) {
    entries_.clear();
    retention_ = retention;
    generation_ = 0U;
    evictions_ = 0U;
}

std::vector<ProcessInfo> ProcessMetadataCache::snapshot() const {
    std::vector<ProcessInfo> result;
    result.reserve(entries_.size());
    for (const auto& [identity, entry] : entries_) {
        static_cast<void>(identity);
        result.push_back(entry.info);
    }
    return result;
}

std::vector<ProcessInfo> ProcessMetadataCache::active_snapshot(
    const std::span<const ProcessSample> active_processes) const {
    std::vector<ProcessInfo> result;
    result.reserve(active_processes.size());
    for (const auto& process : active_processes) {
        const auto iterator = entries_.find(process.identity);
        if (iterator != entries_.end()) {
            result.push_back(iterator->second.info);
        }
    }
    return result;
}

std::size_t ProcessMetadataCache::size() const noexcept {
    return entries_.size();
}

std::size_t ProcessMetadataCache::maximum_entries() const noexcept {
    return maximum_entries_;
}

std::uint64_t ProcessMetadataCache::evictions() const noexcept {
    return evictions_;
}

} // namespace blackbox::telemetry
