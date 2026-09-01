#include "storage/sqlite_incident_archive_internal.hpp"

#include <mutex>

namespace blackbox::storage {

using namespace detail;

std::expected<ProcessProfileStorageStatistics, StorageError>
SqliteIncidentArchive::process_profile_storage_statistics() const noexcept {
    const std::scoped_lock lock{native_->mutex};
    if (native_->database == nullptr) {
        return std::unexpected{
            simple_error(StorageErrorCode::not_open, "incident archive is not open")};
    }
    auto identities = scalar_int64(native_->database, "SELECT COUNT(*) FROM executable_profiles",
                                   "count process profiles");
    if (!identities) return std::unexpected{identities.error()};
    auto observations =
        scalar_int64(native_->database, "SELECT COUNT(*) FROM executable_profile_observations",
                     "count process profile observations");
    if (!observations) return std::unexpected{observations.error()};
    return ProcessProfileStorageStatistics{static_cast<std::uint64_t>(*identities),
                                           static_cast<std::uint64_t>(*observations)};
}

std::expected<std::uint64_t, StorageError> SqliteIncidentArchive::incident_count() const noexcept {
    const std::scoped_lock lock{native_->mutex};
    if (native_->database == nullptr) {
        return std::unexpected{
            simple_error(StorageErrorCode::not_open, "incident archive is not open")};
    }
    auto count =
        scalar_int64(native_->database, "SELECT COUNT(*) FROM incidents", "count incidents");
    if (!count) return std::unexpected{count.error()};
    return static_cast<std::uint64_t>(*count);
}

std::expected<std::int32_t, StorageError> SqliteIncidentArchive::schema_version() const noexcept {
    const std::scoped_lock lock{native_->mutex};
    if (native_->database == nullptr) {
        return std::unexpected{
            simple_error(StorageErrorCode::not_open, "incident archive is not open")};
    }
    auto version = scalar_int64(native_->database, "PRAGMA user_version", "read schema version");
    if (!version) return std::unexpected{version.error()};
    return static_cast<std::int32_t>(*version);
}

std::expected<std::uint64_t, StorageError>
SqliteIncidentArchive::database_size_bytes() const noexcept {
    const std::scoped_lock lock{native_->mutex};
    if (native_->database == nullptr) {
        return std::unexpected{
            simple_error(StorageErrorCode::not_open, "incident archive is not open")};
    }
    auto pages = scalar_int64(native_->database, "PRAGMA page_count", "read page count");
    auto size = scalar_int64(native_->database, "PRAGMA page_size", "read page size");
    if (!pages) return std::unexpected{pages.error()};
    if (!size) return std::unexpected{size.error()};
    return static_cast<std::uint64_t>(*pages) * static_cast<std::uint64_t>(*size);
}

const ArchiveConfiguration& SqliteIncidentArchive::configuration() const noexcept {
    return configuration_;
}

} // namespace blackbox::storage
