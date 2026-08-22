#pragma once

#include "core/incident.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace blackbox::evaluation {

inline constexpr std::uint32_t truth_review_format_version = 1U;
inline constexpr std::size_t maximum_truth_review_system_samples = 20'000U;
inline constexpr std::size_t maximum_truth_review_process_samples = 250'000U;
inline constexpr std::size_t maximum_truth_review_processes = 10'000U;
inline constexpr std::size_t maximum_truth_review_system_events = 65'536U;

struct TruthReviewOptions {
    bool include_local_process_identities{};
};

struct TruthReviewStatistics {
    std::size_t system_samples{};
    std::size_t process_samples{};
    std::size_t processes{};
    std::size_t system_events{};
    bool local_process_identities{};
    friend bool operator==(const TruthReviewStatistics&,
                           const TruthReviewStatistics&) = default;
};

enum class TruthReviewErrorCode : std::uint8_t {
    invalid_input,
    limit_exceeded,
    destination_exists,
    io,
};

struct TruthReviewError {
    TruthReviewErrorCode code{TruthReviewErrorCode::invalid_input};
    std::string message{};
    friend bool operator==(const TruthReviewError&, const TruthReviewError&) = default;
};

// Publishes an exact direct-v1, self-contained, prediction-free review directory.
// The default artifact contains process ordinals only. Local PID/name inclusion is
// an explicit opt-in and never changes the retained incident or dogfood corpus.
[[nodiscard]] std::expected<TruthReviewStatistics, TruthReviewError>
export_truth_review(const core::IncidentSnapshot& incident,
                    std::string_view incident_key,
                    std::int64_t created_utc_milliseconds,
                    const std::filesystem::path& destination,
                    TruthReviewOptions options = {}) noexcept;

} // namespace blackbox::evaluation
