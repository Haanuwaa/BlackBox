#pragma once

#include "storage/incident_archive.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>

namespace blackbox::storage {

struct SqliteIncidentArchive::NativeState {
  mutable std::mutex mutex{};
  sqlite3 *database{};
};

namespace detail {

[[nodiscard]] StorageError database_error(sqlite3 *database,
                                          std::string_view context,
                                          int override_code = SQLITE_OK);
[[nodiscard]] StorageError simple_error(StorageErrorCode code,
                                        std::string_view message);
[[nodiscard]] std::expected<std::int64_t, StorageError>
scalar_int64(sqlite3 *database, const char *sql, std::string_view context);
[[nodiscard]] std::expected<std::string, StorageError>
scalar_text(sqlite3 *database, const char *sql, std::string_view context);
[[nodiscard]] std::expected<void, StorageError>
validate_direct_schema_v1(sqlite3 *database);
[[nodiscard]] std::expected<void, StorageError>
configure_size_limit(sqlite3 *database, std::uint64_t maximum_bytes);

} // namespace detail
} // namespace blackbox::storage
