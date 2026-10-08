#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string_view>

namespace blackbox::storage {
struct ArchiveOperationOptions {
    std::stop_token cancellation{};
    std::chrono::steady_clock::time_point deadline{
        std::chrono::steady_clock::now() + std::chrono::minutes{2}};
    std::function<void(std::string_view, std::uint64_t, std::uint64_t)> progress{};
    [[nodiscard]] bool interrupted() const noexcept {
        return cancellation.stop_requested() || std::chrono::steady_clock::now() >= deadline;
    }
    void report(std::string_view stage, std::uint64_t completed = 0U,
                std::uint64_t total = 0U) const noexcept {
        if (progress) {
            try { progress(stage, completed, total); } catch (...) {}
        }
    }
};
} // namespace blackbox::storage
