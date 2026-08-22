#pragma once

#include "evaluation/dogfood_corpus.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace blackbox::evaluation {

inline constexpr std::uint32_t campaign_status_format_version = 1U;

struct CampaignStatusStatistics {
    std::size_t profiles{};
    std::size_t symptoms{};
    std::size_t unmet_requirements{};
    bool qualification_ready{};
    friend bool operator==(const CampaignStatusStatistics&,
                           const CampaignStatusStatistics&) = default;
};

enum class CampaignStatusErrorCode : std::uint8_t {
    invalid_input,
    destination_exists,
    corpus_invalid,
    io,
};

struct CampaignStatusError {
    CampaignStatusErrorCode code{CampaignStatusErrorCode::invalid_input};
    std::string message{};
    friend bool operator==(const CampaignStatusError&,
                           const CampaignStatusError&) = default;
};

// Publishes an exact schema-v1, prediction-free campaign-readiness directory.
// It summarizes only corpus metadata and qualification gates; it never analyzes
// incident telemetry and cannot count as collection or held-out evidence.
[[nodiscard]] std::expected<CampaignStatusStatistics, CampaignStatusError>
export_campaign_status(const DogfoodCorpus& corpus,
                       std::int64_t created_utc_milliseconds,
                       const std::filesystem::path& destination) noexcept;

} // namespace blackbox::evaluation
