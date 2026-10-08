#include "storage/sqlite_incident_archive_internal.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>
#include <fstream>
#include <memory>
#include <thread>
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

struct ProgressGuard {
  sqlite3* database;
  explicit ProgressGuard(sqlite3* db, const ArchiveOperationOptions& options) : database{db} {
    sqlite3_progress_handler(db, 1000, [](void* context) {
      return static_cast<const ArchiveOperationOptions*>(context)->interrupted() ? 1 : 0;
    }, const_cast<ArchiveOperationOptions*>(&options));
  }
  ~ProgressGuard() { sqlite3_progress_handler(database, 0, nullptr, nullptr); }
};

std::expected<void, StorageError> copy_pages(sqlite3* destination, sqlite3* source,
                                           const ArchiveOperationOptions& options) {
  if (options.interrupted()) return std::unexpected{simple_error(StorageErrorCode::io,
      "Archive operation cancelled or deadline exceeded")};
  auto* backup = sqlite3_backup_init(destination, "main", source, "main");
  if (!backup) return std::unexpected{database_error(destination, "initialize archive copy")};
  int result = SQLITE_OK;
  do {
    if (options.interrupted()) { result = SQLITE_INTERRUPT; break; }
    result = sqlite3_backup_step(backup, 128);
    const auto total = std::max(0, sqlite3_backup_pagecount(backup));
    const auto remaining = std::max(0, sqlite3_backup_remaining(backup));
    options.report("Copying archive", static_cast<std::uint64_t>(std::max(0, total - remaining)),
                   static_cast<std::uint64_t>(total));
    if (result == SQLITE_BUSY || result == SQLITE_LOCKED)
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
  } while (result == SQLITE_OK || result == SQLITE_BUSY || result == SQLITE_LOCKED);
  const auto finished = sqlite3_backup_finish(backup); // Unfinished copies roll back.
  if (result != SQLITE_DONE || finished != SQLITE_OK)
    return std::unexpected{simple_error(StorageErrorCode::io,
        result == SQLITE_INTERRUPT ? "Archive operation cancelled or deadline exceeded" :
                                     "Archive copy did not complete")};
  return {};
}

std::expected<void, StorageError> verify_database(sqlite3* database,
                                                const ArchiveOperationOptions& options) {
  if (options.interrupted()) return std::unexpected{simple_error(StorageErrorCode::io,
      "Archive operation cancelled or deadline exceeded")};
  options.report("Verifying archive");
  ProgressGuard progress{database, options};
  const auto integrity = scalar_text(database, "PRAGMA integrity_check", "verify archive copy");
  if (!integrity || *integrity != "ok") {
    return std::unexpected{simple_error(StorageErrorCode::corrupt,
        "archive copy failed integrity verification")};
  }
  return validate_direct_schema_v1(database);
}

// Source lifetime/transaction is owned by the caller. Restore holds its archive
// mutex; ordinary file backups use an independent read-only connection.
std::expected<void, StorageError> verified_backup(sqlite3* source,
                                                const std::filesystem::path& destination,
                                                const ArchiveOperationOptions& options) {
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
  const auto copied = copy_pages(raw, source, options);
  target.reset();
  if (!copied) return copied;
  const auto reopened = sqlite3_open_v2(core::path_to_utf8(staging).c_str(), &raw,
      SQLITE_OPEN_READONLY, nullptr);
  DatabaseHandle verification{raw, sqlite3_close};
  if (reopened != SQLITE_OK) {
    return std::unexpected{database_error(raw, "reopen archive backup", reopened)};
  }
  if (const auto verified = verify_database(raw, options); !verified) return verified;
  verification.reset();
  if (options.interrupted()) return std::unexpected{simple_error(StorageErrorCode::io,
      "Archive operation cancelled or deadline exceeded before publication")};
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
    const std::filesystem::path& destination, const ArchiveOperationOptions& options) const noexcept {
  try {
    std::unique_lock lock{native_->mutex};
    if (native_->database == nullptr) {
      return std::unexpected{simple_error(StorageErrorCode::not_open, "incident archive is not open")};
    }
    if (destination.empty() || destination == configuration_.path ||
        std::filesystem::exists(std::filesystem::symlink_status(destination))) {
      return std::unexpected{simple_error(StorageErrorCode::invalid_data,
          "backup destination must be a new file distinct from the archive")};
    }
    if (configuration_.path == std::filesystem::path{":memory:"})
      return verified_backup(native_->database, destination, options);
    sqlite3* raw{};
    const auto opened = sqlite3_open_v2(core::path_to_utf8(configuration_.path).c_str(), &raw,
                                      SQLITE_OPEN_READONLY, nullptr);
    DatabaseHandle source{raw, sqlite3_close};
    if (opened != SQLITE_OK) return std::unexpected{database_error(raw, "open backup reader", opened)};
    sqlite3_busy_timeout(raw, 100);
    if (auto begun = execute(raw, "BEGIN", "begin backup snapshot"); !begun) return begun;
    if (auto pinned = scalar_int64(raw, "PRAGMA schema_version", "pin backup snapshot"); !pinned)
      return std::unexpected{pinned.error()};
    // The read transaction freezes this copy in WAL mode. Writers use their own
    // connection while copy and validation run; the deadline bounds WAL pinning.
    lock.unlock();
    return verified_backup(raw, destination, options);
  } catch (const std::exception& exception) {
    return std::unexpected{StorageError{StorageErrorCode::io, 0, exception.what()}};
  } catch (...) {
    return std::unexpected{simple_error(StorageErrorCode::io, "unknown archive backup failure")};
  }
}

std::expected<void, StorageError> SqliteIncidentArchive::restore_from(
    const std::filesystem::path &source,
    const std::filesystem::path &safety_backup, const ArchiveOperationOptions& options) noexcept {
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
    if (const auto verified = verify_database(input, options); !verified) {
      return std::unexpected{simple_error(StorageErrorCode::corrupt,
          "restore source failed integrity or schema validation")};
    }
    if (const auto preserved = verified_backup(native_->database, safety_backup, options); !preserved) {
      return std::unexpected{preserved.error()};
    }
    const auto restored = copy_pages(native_->database, input, options);
    source_handle.reset();
    if (!restored) return restored;
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
