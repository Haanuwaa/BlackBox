#pragma once

#include "storage/incident_archive.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>

namespace blackbox::storage {

inline constexpr std::uint32_t incident_dataset_format_version = 1U;

struct IncidentDatasetStatistics {
    std::uint64_t incidents{};
    std::uint64_t system_samples{};
    std::uint64_t process_samples{};
    std::uint64_t system_events{};
    std::uint64_t classification_events{};
    std::uint64_t classifications_updated{};
    friend bool operator==(const IncidentDatasetStatistics&,
                           const IncidentDatasetStatistics&) = default;
};

[[nodiscard]] std::expected<IncidentDatasetStatistics, StorageError>
export_incident_dataset(SqliteIncidentArchive& archive,
                        const std::filesystem::path& destination) noexcept;

// Imports only category and noticed/not-noticed feedback for matching export keys.
// Recorded telemetry, free-form labels, notes, and process metadata are immutable.
[[nodiscard]] std::expected<IncidentDatasetStatistics, StorageError>
import_incident_dataset_classifications(
    SqliteIncidentArchive& archive,
    const std::filesystem::path& source) noexcept;

} // namespace blackbox::storage
