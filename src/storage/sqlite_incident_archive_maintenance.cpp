#include "storage/sqlite_incident_archive_internal.hpp"

#include <sqlite3.h>

#include <exception>
#include <filesystem>
#include <string>
#include <utility>

namespace blackbox::storage {

using namespace detail;

std::expected<void, StorageError> SqliteIncidentArchive::backup_to(
    const std::filesystem::path &destination) const noexcept {
  try {
    const std::scoped_lock lock{native_->mutex};
    if (native_->database == nullptr) {
      return std::unexpected{simple_error(StorageErrorCode::not_open,
                                          "incident archive is not open")};
    }
    if (destination.empty() || destination == configuration_.path ||
        std::filesystem::exists(destination)) {
      return std::unexpected{simple_error(
          StorageErrorCode::invalid_data,
          "backup destination must be a new file distinct from the archive")};
    }
    if (!destination.parent_path().empty()) {
      std::filesystem::create_directories(destination.parent_path());
    }
    sqlite3 *target{};
#if defined(_WIN32)
    const auto opened = sqlite3_open16(destination.native().c_str(), &target);
#else
    const auto opened = sqlite3_open(destination.string().c_str(), &target);
#endif
    if (opened != SQLITE_OK) {
      auto error = database_error(target, "open backup destination", opened);
      if (target != nullptr)
        sqlite3_close(target);
      return std::unexpected{std::move(error)};
    }
    auto *backup =
        sqlite3_backup_init(target, "main", native_->database, "main");
    if (backup == nullptr) {
      auto error = database_error(target, "initialize archive backup");
      sqlite3_close(target);
      std::filesystem::remove(destination);
      return std::unexpected{std::move(error)};
    }
    const auto stepped = sqlite3_backup_step(backup, -1);
    const auto finished = sqlite3_backup_finish(backup);
    const auto closed = sqlite3_close(target);
    if ((stepped != SQLITE_DONE && stepped != SQLITE_OK) ||
        finished != SQLITE_OK || closed != SQLITE_OK) {
      std::filesystem::remove(destination);
      return std::unexpected{simple_error(StorageErrorCode::io,
                                          "archive backup did not complete")};
    }
    return {};
  } catch (const std::exception &exception) {
    return std::unexpected{
        StorageError{StorageErrorCode::io, 0, exception.what()}};
  } catch (...) {
    return std::unexpected{
        simple_error(StorageErrorCode::io, "unknown archive backup failure")};
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
    const auto source_native_utf8 = source.u8string();
    const std::string source_utf8{source_native_utf8.begin(),
                                  source_native_utf8.end()};
    const auto opened = sqlite3_open_v2(source_utf8.c_str(), &input,
                                        SQLITE_OPEN_READONLY, nullptr);
    if (opened != SQLITE_OK) {
      auto error = database_error(input, "open restore source", opened);
      if (input != nullptr)
        sqlite3_close(input);
      return std::unexpected{std::move(error)};
    }
    const auto integrity =
        scalar_text(input, "PRAGMA integrity_check", "verify restore source");
    const auto version = scalar_int64(input, "PRAGMA user_version",
                                      "read restore schema version");
    const auto direct_schema =
        version && *version == current_schema_version
            ? validate_direct_schema_v1(input)
            : std::expected<void, StorageError>{std::unexpected{
                  simple_error(StorageErrorCode::invalid_schema,
                               "restore source is not direct schema v1")}};
    if (!integrity || *integrity != "ok" || !version ||
        *version != current_schema_version || !direct_schema) {
      sqlite3_close(input);
      return std::unexpected{
          simple_error(StorageErrorCode::corrupt,
                       "restore source failed integrity or schema validation")};
    }
    if (!safety_backup.parent_path().empty()) {
      std::filesystem::create_directories(safety_backup.parent_path());
    }
    sqlite3 *safety{};
#if defined(_WIN32)
    const auto safety_opened =
        sqlite3_open16(safety_backup.native().c_str(), &safety);
#else
    const auto safety_opened =
        sqlite3_open(safety_backup.string().c_str(), &safety);
#endif
    if (safety_opened != SQLITE_OK) {
      auto error =
          database_error(safety, "open restore safety backup", safety_opened);
      if (safety != nullptr)
        sqlite3_close(safety);
      sqlite3_close(input);
      return std::unexpected{std::move(error)};
    }
    auto *preserve =
        sqlite3_backup_init(safety, "main", native_->database, "main");
    const auto preserve_step =
        preserve == nullptr ? SQLITE_ERROR : sqlite3_backup_step(preserve, -1);
    const auto preserve_finish =
        preserve == nullptr ? SQLITE_ERROR : sqlite3_backup_finish(preserve);
    if (preserve == nullptr || preserve_step != SQLITE_DONE ||
        preserve_finish != SQLITE_OK) {
      sqlite3_close(safety);
      sqlite3_close(input);
      std::filesystem::remove(safety_backup);
      return std::unexpected{simple_error(
          StorageErrorCode::io, "could not create restore safety backup")};
    }
    sqlite3_close(safety);
    auto *restore =
        sqlite3_backup_init(native_->database, "main", input, "main");
    if (restore == nullptr) {
      sqlite3_close(input);
      return std::unexpected{
          database_error(native_->database, "initialize archive restore")};
    }
    const auto stepped = sqlite3_backup_step(restore, -1);
    const auto finished = sqlite3_backup_finish(restore);
    sqlite3_close(input);
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
