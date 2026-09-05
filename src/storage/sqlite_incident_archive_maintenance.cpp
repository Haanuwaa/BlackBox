#include "storage/sqlite_incident_archive_internal.hpp"

#include <sqlite3.h>

#include <exception>
#include <filesystem>
#include <string>
#include <utility>
#include <fstream>
#include <memory>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif
#include "core/filesystem_text.hpp"

namespace blackbox::storage {

using namespace detail;

namespace {

using DatabaseHandle = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

std::expected<void, StorageError> verify_database(sqlite3* database) {
  const auto integrity = scalar_text(database, "PRAGMA integrity_check", "verify archive copy");
  if (!integrity || *integrity != "ok") {
    return std::unexpected{simple_error(StorageErrorCode::corrupt,
        "archive copy failed integrity verification")};
  }
  return validate_direct_schema_v1(database);
}

// Called with the archive mutex held, also for the mandatory pre-restore copy.
std::expected<void, StorageError> verified_backup(sqlite3* source,
                                                const std::filesystem::path& destination) {
  if (!destination.parent_path().empty()) {
    std::filesystem::create_directories(destination.parent_path());
  }
  auto staging = destination;
  staging += ".partial";
  // Exclusive creation also refuses dangling links and competing backup jobs.
  std::ofstream reservation{staging, std::ios::binary | std::ios::out | std::ios::noreplace};
  if (!reservation) {
    return std::unexpected{simple_error(StorageErrorCode::invalid_data,
        "backup staging file already exists or cannot be created")};
  }
  reservation.close();
  struct StagingCleanup {
    const std::filesystem::path& path;
    ~StagingCleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
  } cleanup{staging};
  sqlite3* raw{};
  const auto opened = sqlite3_open_v2(core::path_to_utf8(staging).c_str(), &raw,
      SQLITE_OPEN_READWRITE, nullptr);
  DatabaseHandle target{raw, sqlite3_close};
  if (opened != SQLITE_OK) {
    return std::unexpected{database_error(raw, "open backup destination", opened)};
  }
  auto* backup = sqlite3_backup_init(raw, "main", source, "main");
  if (backup == nullptr) {
    return std::unexpected{database_error(raw, "initialize archive backup")};
  }
  const auto stepped = sqlite3_backup_step(backup, -1);
  const auto finished = sqlite3_backup_finish(backup);
  target.reset();
  if (stepped != SQLITE_DONE || finished != SQLITE_OK) {
    return std::unexpected{simple_error(StorageErrorCode::io, "archive backup did not complete")};
  }
  const auto reopened = sqlite3_open_v2(core::path_to_utf8(staging).c_str(), &raw,
      SQLITE_OPEN_READONLY, nullptr);
  DatabaseHandle verification{raw, sqlite3_close};
  if (reopened != SQLITE_OK) {
    return std::unexpected{database_error(raw, "reopen archive backup", reopened)};
  }
  if (const auto verified = verify_database(raw); !verified) return verified;
  verification.reset();
#if defined(_WIN32)
  // No REPLACE_EXISTING: supports NTFS and removable FAT/exFAT destinations.
  if (!MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
    return std::unexpected{simple_error(StorageErrorCode::io,
        "could not publish verified backup; destination must be a new writable file")};
  }
#else
  // Same-directory link publication is atomic and refuses an existing destination.
  std::error_code error;
  std::filesystem::create_hard_link(staging, destination, error);
  if (error) {
    return std::unexpected{simple_error(StorageErrorCode::io,
        "could not publish verified backup exclusively on this filesystem")};
  }
#endif
  return {};
}

} // namespace

std::expected<void, StorageError> SqliteIncidentArchive::backup_to(
    const std::filesystem::path& destination) const noexcept {
  try {
    const std::scoped_lock lock{native_->mutex};
    if (native_->database == nullptr) {
      return std::unexpected{simple_error(StorageErrorCode::not_open, "incident archive is not open")};
    }
    if (destination.empty() || destination == configuration_.path ||
        std::filesystem::exists(std::filesystem::symlink_status(destination))) {
      return std::unexpected{simple_error(StorageErrorCode::invalid_data,
          "backup destination must be a new file distinct from the archive")};
    }
    return verified_backup(native_->database, destination);
  } catch (const std::exception& exception) {
    return std::unexpected{StorageError{StorageErrorCode::io, 0, exception.what()}};
  } catch (...) {
    return std::unexpected{simple_error(StorageErrorCode::io, "unknown archive backup failure")};
  }
}

std::expected<void, StorageError> SqliteIncidentArchive::restore_from(
    const std::filesystem::path &source,
    const std::filesystem::path &safety_backup) noexcept {
  try {
    if (source.empty() || safety_backup.empty() ||
        source == configuration_.path || safety_backup == configuration_.path ||
        source == safety_backup || !std::filesystem::exists(source) ||
        std::filesystem::exists(safety_backup)) {
      return std::unexpected{
          simple_error(StorageErrorCode::invalid_data,
                       "restore requires an existing source and a new distinct "
                       "safety-backup file")};
    }
    const std::scoped_lock lock{native_->mutex};
    if (native_->database == nullptr) {
      return std::unexpected{simple_error(StorageErrorCode::not_open,
                                          "incident archive is not open")};
    }
    sqlite3 *input{};
    const auto source_utf8 = core::path_to_utf8(source);
    const auto opened = sqlite3_open_v2(source_utf8.c_str(), &input,
                                        SQLITE_OPEN_READONLY, nullptr);
    DatabaseHandle source_handle{input, sqlite3_close};
    if (opened != SQLITE_OK) {
      auto error = database_error(input, "open restore source", opened);
      return std::unexpected{std::move(error)};
    }
    if (const auto verified = verify_database(input); !verified) {
      return std::unexpected{simple_error(StorageErrorCode::corrupt,
          "restore source failed integrity or schema validation")};
    }
    if (const auto preserved = verified_backup(native_->database, safety_backup); !preserved) {
      return std::unexpected{preserved.error()};
    }
    auto *restore =
        sqlite3_backup_init(native_->database, "main", input, "main");
    if (restore == nullptr) {
      return std::unexpected{
          database_error(native_->database, "initialize archive restore")};
    }
    const auto stepped = sqlite3_backup_step(restore, -1);
    const auto finished = sqlite3_backup_finish(restore);
    source_handle.reset();
    if ((stepped != SQLITE_DONE && stepped != SQLITE_OK) ||
        finished != SQLITE_OK) {
      return std::unexpected{simple_error(StorageErrorCode::io,
                                          "archive restore did not complete")};
    }
    if (auto limited = configure_size_limit(native_->database,
                                            configuration_.maximum_bytes);
        !limited)
      return std::unexpected{limited.error()};
    return {};
  } catch (const std::exception &exception) {
    return std::unexpected{
        StorageError{StorageErrorCode::io, 0, exception.what()}};
  } catch (...) {
    return std::unexpected{
        simple_error(StorageErrorCode::io, "unknown archive restore failure")};
  }
}

} // namespace blackbox::storage
