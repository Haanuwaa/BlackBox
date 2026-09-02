#pragma once

#include "telemetry/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace blackbox::telemetry {

// Fixed-capacity state for compositor surfaces that expose an active
// application token without a trustworthy process identity. Native strings
// and handles stay outside this type; only already-opaque nonzero tokens enter.
template <std::size_t Capacity> class ForegroundApplicationTracker final {
public:
    [[nodiscard]] bool add(const std::uint64_t handle) noexcept {
        if (handle == 0U) return false;
        if (find(handle) != nullptr) return true;
        for (auto& entry : entries_) {
            if (entry.handle == 0U) {
                entry.handle = handle;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool set_application_token(const std::uint64_t handle,
                                             const std::uint64_t token) noexcept {
        auto* entry = find(handle);
        if (entry == nullptr || token == 0U) return false;
        entry->application_token = token;
        return true;
    }

    [[nodiscard]] bool set_active(const std::uint64_t handle, const bool active) noexcept {
        auto* entry = find(handle);
        if (entry == nullptr) return false;
        entry->active = active;
        return true;
    }

    void remove(const std::uint64_t handle) noexcept {
        if (auto* entry = find(handle); entry != nullptr) *entry = {};
    }

    void reset() noexcept { entries_ = {}; }

    [[nodiscard]] MetricValue<OpaqueApplicationIdentity>
    current(const std::uint64_t session_token, const MetricStatus source_status) const noexcept {
        if (source_status != MetricStatus::available) {
            return MetricValue<OpaqueApplicationIdentity>::unavailable(source_status);
        }
        std::uint64_t selected_token{};
        for (const auto& entry : entries_) {
            if (!entry.active || entry.application_token == 0U) continue;
            if (selected_token != 0U && selected_token != entry.application_token) {
                return MetricValue<OpaqueApplicationIdentity>::unavailable(
                    MetricStatus::temporarily_unavailable);
            }
            selected_token = entry.application_token;
        }
        if (selected_token == 0U || session_token == 0U) {
            return MetricValue<OpaqueApplicationIdentity>::unavailable(
                MetricStatus::temporarily_unavailable);
        }
        return MetricValue<OpaqueApplicationIdentity>::available(
            OpaqueApplicationIdentity{session_token, selected_token});
    }

private:
    struct Entry {
        std::uint64_t handle{};
        std::uint64_t application_token{};
        bool active{};
    };

    [[nodiscard]] Entry* find(const std::uint64_t handle) noexcept {
        for (auto& entry : entries_) {
            if (entry.handle == handle) return &entry;
        }
        return nullptr;
    }

    [[nodiscard]] const Entry* find(const std::uint64_t handle) const noexcept {
        for (const auto& entry : entries_) {
            if (entry.handle == handle) return &entry;
        }
        return nullptr;
    }

    std::array<Entry, Capacity> entries_{};
};

} // namespace blackbox::telemetry
