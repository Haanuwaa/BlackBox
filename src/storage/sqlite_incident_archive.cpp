#include "storage/incident_archive.hpp"
#include "storage/archive_schema.hpp"
#include "storage/sqlite_incident_archive_internal.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace blackbox::storage {
namespace detail {

[[nodiscard]] StorageErrorCode classify_error(const int code) noexcept {
    switch (code & 0xFF) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return StorageErrorCode::busy;
    case SQLITE_FULL:
        return StorageErrorCode::full;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
        return StorageErrorCode::corrupt;
    case SQLITE_CANTOPEN:
    case SQLITE_PERM:
    case SQLITE_READONLY:
        return StorageErrorCode::cannot_open;
    case SQLITE_IOERR:
        return StorageErrorCode::io;
    case SQLITE_CONSTRAINT:
    case SQLITE_MISMATCH:
        return StorageErrorCode::invalid_data;
    case SQLITE_SCHEMA:
        return StorageErrorCode::invalid_schema;
    default:
        return StorageErrorCode::sql_error;
    }
}

[[nodiscard]] StorageError database_error(sqlite3* database,
                                          const std::string_view context,
                                          const int override_code) {
    const auto code = override_code == SQLITE_OK
                          ? sqlite3_extended_errcode(database)
                          : override_code;
    std::string message{context};
    message += ": ";
    message += database != nullptr ? sqlite3_errmsg(database) : sqlite3_errstr(code);
    return {classify_error(code), code, std::move(message)};
}

[[nodiscard]] StorageError simple_error(const StorageErrorCode code,
                                        const std::string_view message) {
    return {code, 0, std::string{message}};
}

[[nodiscard]] std::expected<void, StorageError> validate_existing_archive_header(
    const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path))) {
        return std::unexpected{simple_error(StorageErrorCode::cannot_open,
                                             "archive is not a regular file")};
    }
    if (std::filesystem::file_size(path) == 0U) return {};

    constexpr std::array<char, 16U> sqlite_header{
        'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f',
        'o', 'r', 'm', 'a', 't', ' ', '3', '\0'};
    std::array<char, sqlite_header.size()> actual{};
    std::ifstream input{path, std::ios::binary};
    input.read(actual.data(), static_cast<std::streamsize>(actual.size()));
    if (input.gcount() != static_cast<std::streamsize>(actual.size()) ||
        actual != sqlite_header) {
        return std::unexpected{simple_error(
            StorageErrorCode::corrupt,
            "existing archive does not have a valid SQLite header")};
    }
    return {};
}

[[nodiscard]] std::optional<std::string> environment_value(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t size = 0U;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result{value};
    std::free(value);
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
#else
    const auto* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string{value};
#endif
}

[[nodiscard]] std::expected<void, StorageError> execute(
    sqlite3* database, const char* sql, const std::string_view context) {
    char* error_message = nullptr;
    const auto result = sqlite3_exec(database, sql, nullptr, nullptr, &error_message);
    if (result == SQLITE_OK) {
        return {};
    }
    std::string message{context};
    message += ": ";
    if (error_message != nullptr) {
        message += error_message;
        sqlite3_free(error_message);
    } else {
        message += sqlite3_errmsg(database);
    }
    return std::unexpected{StorageError{classify_error(result), result,
                                         std::move(message)}};
}

class Statement final {
public:
    Statement() = default;
    explicit Statement(sqlite3_stmt* statement) noexcept : statement_{statement} {}
    ~Statement() {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept
        : statement_{std::exchange(other.statement_, nullptr)} {}
    Statement& operator=(Statement&& other) noexcept {
        if (this != &other) {
            if (statement_ != nullptr) {
                sqlite3_finalize(statement_);
            }
            statement_ = std::exchange(other.statement_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3_stmt* statement_{};
};

[[nodiscard]] std::expected<Statement, StorageError> prepare(
    sqlite3* database, const char* sql, const std::string_view context) {
    sqlite3_stmt* statement = nullptr;
    const auto result = sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
    if (result != SQLITE_OK) {
        return std::unexpected{database_error(database, context, result)};
    }
    return Statement{statement};
}

class Transaction final {
public:
    explicit Transaction(sqlite3* database) noexcept : database_{database} {}
    ~Transaction() {
        if (active_) {
            static_cast<void>(sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr));
        }
    }
    [[nodiscard]] std::expected<void, StorageError> begin() {
        auto result = execute(database_, "BEGIN IMMEDIATE", "begin incident transaction");
        active_ = result.has_value();
        return result;
    }
    [[nodiscard]] std::expected<void, StorageError> commit() {
        auto result = execute(database_, "COMMIT", "commit incident transaction");
        if (result.has_value()) {
            active_ = false;
        }
        return result;
    }

private:
    sqlite3* database_{};
    bool active_{};
};

[[nodiscard]] std::int64_t monotonic_nanoseconds(
    const core::MonotonicTimePoint time) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               time.time_since_epoch()).count();
}

[[nodiscard]] core::MonotonicTimePoint monotonic_time(const std::int64_t value) noexcept {
    return core::MonotonicTimePoint{
        std::chrono::duration_cast<core::MonotonicClock::duration>(
            std::chrono::nanoseconds{value})};
}

[[nodiscard]] std::array<unsigned char, 8U> unsigned_bytes(
    const std::uint64_t value) noexcept {
    std::array<unsigned char, 8U> result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const auto shift = static_cast<unsigned int>((result.size() - 1U - index) * 8U);
        result[index] = static_cast<unsigned char>((value >> shift) & 0xFFU);
    }
    return result;
}

int bind_unsigned(sqlite3_stmt* statement, const int index,
                  const std::uint64_t value) noexcept {
    const auto bytes = unsigned_bytes(value);
    return sqlite3_bind_blob(statement, index, bytes.data(),
                             static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
}

[[nodiscard]] std::expected<std::uint64_t, StorageError> read_unsigned(
    sqlite3_stmt* statement, const int column, const std::string_view field) {
    const auto size = sqlite3_column_bytes(statement, column);
    const auto* bytes = static_cast<const unsigned char*>(
        sqlite3_column_blob(statement, column));
    if (size != 8 || bytes == nullptr) {
        return std::unexpected{simple_error(
            StorageErrorCode::invalid_data,
            std::string{"invalid unsigned field: "} + std::string{field})};
    }
    std::uint64_t result = 0U;
    for (int index = 0; index < size; ++index) {
        result = (result << 8U) | bytes[index];
    }
    return result;
}

[[nodiscard]] std::expected<IncidentExportKey, StorageError> read_export_key(
    sqlite3_stmt* statement, const int column) {
    const auto size = sqlite3_column_bytes(statement, column);
    const auto* bytes = static_cast<const std::uint8_t*>(
        sqlite3_column_blob(statement, column));
    if (size != static_cast<int>(IncidentExportKey{}.bytes.size()) || bytes == nullptr) {
        return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                             "invalid incident export key")};
    }
    IncidentExportKey key{};
    std::memcpy(key.bytes.data(), bytes, key.bytes.size());
    return key;
}

[[nodiscard]] constexpr int stored_status(
    const core::RecordedValueStatus status) noexcept {
    return static_cast<int>(status);
}

[[nodiscard]] std::expected<core::RecordedValueStatus, StorageError> read_status(
    sqlite3_stmt* statement, const int column) {
    const auto value = sqlite3_column_int(statement, column);
    if (value < static_cast<int>(core::RecordedValueStatus::available) ||
        value > static_cast<int>(core::RecordedValueStatus::temporarily_unavailable)) {
        return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                             "invalid recorded value status")};
    }
    return static_cast<core::RecordedValueStatus>(value);
}

int bind_double_value(sqlite3_stmt* statement, const int status_index,
                      const int value_index,
                      const core::RecordedValue<double>& value) noexcept {
    auto result = sqlite3_bind_int(statement, status_index, stored_status(value.status));
    if (result == SQLITE_OK) {
        result = value.status == core::RecordedValueStatus::available
                     ? sqlite3_bind_double(statement, value_index, value.value)
                     : sqlite3_bind_null(statement, value_index);
    }
    return result;
}

int bind_unsigned_value(
    sqlite3_stmt* statement, const int status_index, const int value_index,
    const core::RecordedValue<std::uint64_t>& value) noexcept {
    auto result = sqlite3_bind_int(statement, status_index, stored_status(value.status));
    if (result == SQLITE_OK) {
        result = value.status == core::RecordedValueStatus::available
                     ? bind_unsigned(statement, value_index, value.value)
                     : sqlite3_bind_null(statement, value_index);
    }
    return result;
}

int bind_byte_value(sqlite3_stmt* statement, const int status_index,
                    const int value_index,
                    const core::RecordedValue<std::uint8_t>& value) noexcept {
    auto result = sqlite3_bind_int(statement, status_index, stored_status(value.status));
    if (result == SQLITE_OK) {
        result = value.status == core::RecordedValueStatus::available
                     ? sqlite3_bind_int(statement, value_index,
                                        static_cast<int>(value.value))
                     : sqlite3_bind_null(statement, value_index);
    }
    return result;
}

int bind_pid_value(sqlite3_stmt* statement, const int status_index,
                   const int value_index,
                   const core::RecordedValue<std::uint32_t>& value) noexcept {
    auto result = sqlite3_bind_int(statement, status_index, stored_status(value.status));
    if (result == SQLITE_OK) {
        result = value.status == core::RecordedValueStatus::available
                     ? sqlite3_bind_int64(statement, value_index,
                                          static_cast<sqlite3_int64>(value.value))
                     : sqlite3_bind_null(statement, value_index);
    }
    return result;
}

int bind_text_value(sqlite3_stmt* statement, const int status_index,
                    const int value_index,
                    const core::RecordedValue<std::string>& value) noexcept {
    auto result = sqlite3_bind_int(statement, status_index, stored_status(value.status));
    if (result == SQLITE_OK) {
        result = value.status == core::RecordedValueStatus::available
                     ? sqlite3_bind_text(statement, value_index, value.value.c_str(),
                                         static_cast<int>(value.value.size()), SQLITE_TRANSIENT)
                     : sqlite3_bind_null(statement, value_index);
    }
    return result;
}

[[nodiscard]] std::expected<void, StorageError> expect_done(
    sqlite3* database, sqlite3_stmt* statement, const std::string_view context) {
    const auto result = sqlite3_step(statement);
    if (result != SQLITE_DONE) {
        return std::unexpected{database_error(database, context, result)};
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return {};
}

[[nodiscard]] std::expected<std::int64_t, StorageError> scalar_int64(
    sqlite3* database, const char* sql, const std::string_view context) {
    auto statement = prepare(database, sql, context);
    if (!statement) {
        return std::unexpected{statement.error()};
    }
    const auto result = sqlite3_step(statement->get());
    if (result != SQLITE_ROW) {
        return std::unexpected{database_error(database, context, result)};
    }
    return sqlite3_column_int64(statement->get(), 0);
}

[[nodiscard]] std::expected<std::string, StorageError> scalar_text(
    sqlite3* database, const char* sql, const std::string_view context) {
    auto statement = prepare(database, sql, context);
    if (!statement) return std::unexpected{statement.error()};
    const auto result = sqlite3_step(statement->get());
    if (result != SQLITE_ROW) {
        return std::unexpected{database_error(database, context, result)};
    }
    const auto* value = sqlite3_column_text(statement->get(), 0);
    return value == nullptr ? std::string{} :
        std::string{reinterpret_cast<const char*>(value)};
}

struct DirectSchemaObject final {
    std::string type{};
    std::string name{};
    std::string table{};
    std::string sql{};
    friend bool operator==(const DirectSchemaObject&,
                           const DirectSchemaObject&) = default;
};

[[nodiscard]] std::expected<std::vector<DirectSchemaObject>, StorageError>
read_direct_schema_manifest(sqlite3* database, const std::string_view context) {
    auto statement = prepare(database, R"sql(
SELECT type,name,tbl_name,sql
FROM sqlite_schema
WHERE name NOT LIKE 'sqlite_%'
ORDER BY type,name
)sql", context);
    if (!statement) return std::unexpected{statement.error()};

    std::vector<DirectSchemaObject> result{};
    for (;;) {
        const auto stepped = sqlite3_step(statement->get());
        if (stepped == SQLITE_DONE) return result;
        if (stepped != SQLITE_ROW) {
            return std::unexpected{database_error(database, context, stepped)};
        }
        const auto text_column = [raw = statement->get()](const int column) {
            const auto* value = sqlite3_column_text(raw, column);
            return value == nullptr
                       ? std::string{}
                       : std::string{reinterpret_cast<const char*>(value)};
        };
        result.push_back({text_column(0), text_column(1), text_column(2),
                          text_column(3)});
    }
}

[[nodiscard]] std::expected<void, StorageError> validate_direct_schema_v1(
    sqlite3* database) {
    const auto application_id = scalar_int64(database, "PRAGMA application_id",
                                             "validate archive application id");
    if (!application_id) return std::unexpected{application_id.error()};
    if (*application_id != 1111644209) {
        return std::unexpected{simple_error(
            StorageErrorCode::invalid_schema,
            "archive does not match the complete pre-release version-1 baseline; "
            "development archives must be recreated")};
    }

    sqlite3* canonical_database = nullptr;
    const auto opened = sqlite3_open_v2(
        ":memory:", &canonical_database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    if (opened != SQLITE_OK) {
        auto error = database_error(canonical_database,
                                    "create canonical direct-v1 schema", opened);
        if (canonical_database != nullptr) sqlite3_close(canonical_database);
        return std::unexpected{std::move(error)};
    }
    const auto created = execute(canonical_database, archive_schema_v1,
                                 "create canonical direct-v1 schema");
    if (!created) {
        auto error = created.error();
        sqlite3_close(canonical_database);
        return std::unexpected{std::move(error)};
    }
    auto canonical = read_direct_schema_manifest(
        canonical_database, "read canonical direct-v1 schema manifest");
    sqlite3_close(canonical_database);
    if (!canonical) return std::unexpected{canonical.error()};

    const auto actual = read_direct_schema_manifest(
        database, "read archive direct-v1 schema manifest");
    if (!actual) return std::unexpected{actual.error()};
    if (*actual != *canonical) {
        return std::unexpected{simple_error(
            StorageErrorCode::invalid_schema,
            "archive does not match the complete pre-release version-1 baseline; "
            "development archives must be recreated")};
    }

    const auto metadata_rows = scalar_int64(
        database,
        "SELECT count(*) FROM schema_metadata "
        "WHERE singleton=1 AND schema_version=1",
        "validate direct version-1 metadata row");
    if (!metadata_rows) return std::unexpected{metadata_rows.error()};
    const auto metadata_total = scalar_int64(database,
        "SELECT count(*) FROM schema_metadata",
        "validate direct version-1 metadata row count");
    if (!metadata_total) return std::unexpected{metadata_total.error()};
    const auto feedback_rows = scalar_int64(database,
        "SELECT count(*) FROM feedback_profile_state WHERE singleton=1",
        "validate direct version-1 feedback state row");
    if (!feedback_rows) return std::unexpected{feedback_rows.error()};
    if (*metadata_rows != 1 || *metadata_total != 1 || *feedback_rows != 1) {
        return std::unexpected{simple_error(
            StorageErrorCode::invalid_schema,
            "archive is missing required direct version-1 control state; "
            "development archives must be recreated")};
    }
    return {};
}

[[nodiscard]] std::expected<void, StorageError> initialize_schema(sqlite3* database) {
    const auto version = scalar_int64(database, "PRAGMA user_version",
                                      "read schema version");
    if (!version) return std::unexpected{version.error()};
    if (*version == current_schema_version)
        return validate_direct_schema_v1(database);
    if (*version != 0) {
        return std::unexpected{simple_error(
            *version > current_schema_version ? StorageErrorCode::schema_too_new
                                              : StorageErrorCode::invalid_schema,
            "archive schema does not match the pre-release version-1 baseline")};
    }
    const auto objects = scalar_int64(
        database,
        "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'",
        "inspect empty archive");
    if (!objects) return std::unexpected{objects.error()};
    if (*objects != 0) {
        return std::unexpected{simple_error(
            StorageErrorCode::invalid_schema,
            "unversioned non-empty archives are not supported before release")};
    }
    Transaction transaction{database};
    if (auto begun = transaction.begin(); !begun) return begun;
    if (auto created = execute(database, archive_schema_v1,
                               "initialize archive schema version 1");
        !created) return created;
    return transaction.commit();
}
[[nodiscard]] std::expected<void, StorageError> configure_connection(
    sqlite3* database, const ArchiveConfiguration& configuration) {
    sqlite3_extended_result_codes(database, 1);
    sqlite3_busy_timeout(database, static_cast<int>(std::clamp<std::int64_t>(
        configuration.busy_timeout.count(), 0, std::numeric_limits<int>::max())));
    if (auto result = execute(database, "PRAGMA foreign_keys=ON", "enable foreign keys");
        !result) {
        return result;
    }
    if (auto result = execute(database, "PRAGMA secure_delete=ON", "enable secure delete");
        !result) {
        return result;
    }
    if (auto result = execute(database, "PRAGMA synchronous=FULL", "set durability");
        !result) {
        return result;
    }
    return {};
}

[[nodiscard]] std::expected<void, StorageError> configure_read_only_connection(
    sqlite3* database, const ArchiveConfiguration& configuration) {
    sqlite3_extended_result_codes(database, 1);
    sqlite3_busy_timeout(database, static_cast<int>(std::clamp<std::int64_t>(
        configuration.busy_timeout.count(), 0, std::numeric_limits<int>::max())));
    if (auto result = execute(database, "PRAGMA foreign_keys=ON", "enable foreign keys");
        !result) {
        return result;
    }
    if (auto result = execute(database, "PRAGMA query_only=ON", "enforce read-only queries");
        !result) {
        return result;
    }
    const auto version = scalar_int64(database, "PRAGMA user_version",
                                      "read schema version");
    if (!version) return std::unexpected{version.error()};
    if (*version != current_schema_version) {
        return std::unexpected{simple_error(
            *version > current_schema_version ? StorageErrorCode::schema_too_new
                                              : StorageErrorCode::invalid_schema,
            "archive schema does not match the direct version-1 baseline")};
    }
    return validate_direct_schema_v1(database);
}

[[nodiscard]] std::expected<void, StorageError> configure_journal(sqlite3* database) {
    if (auto result = execute(database, "PRAGMA journal_mode=WAL", "enable WAL mode");
        !result) {
        return result;
    }
    if (auto result = execute(database, "PRAGMA wal_autocheckpoint=1000",
                              "set WAL checkpoint policy"); !result) {
        return result;
    }
    return {};
}

[[nodiscard]] std::expected<void, StorageError> configure_size_limit(
    sqlite3* database, const std::uint64_t maximum_bytes) {
    if (maximum_bytes == 0U) {
        return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                             "archive maximum size must be positive")};
    }
    const auto page_size = scalar_int64(database, "PRAGMA page_size", "read page size");
    const auto page_count = scalar_int64(database, "PRAGMA page_count", "read page count");
    if (!page_size) {
        return std::unexpected{page_size.error()};
    }
    if (!page_count) {
        return std::unexpected{page_count.error()};
    }
    const auto requested_pages = static_cast<std::uint64_t>(*page_size) == 0U
                                     ? 0U
                                     : maximum_bytes / static_cast<std::uint64_t>(*page_size);
    const auto maximum_pages = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(*page_count), std::max<std::uint64_t>(1U, requested_pages));
    const auto pragma = "PRAGMA max_page_count=" + std::to_string(maximum_pages);
    return execute(database, pragma.c_str(), "set archive size limit");
}

[[nodiscard]] std::expected<core::RecordedValue<double>, StorageError>
read_double_value(sqlite3_stmt* statement, const int status_column,
                  const int value_column) {
    auto status = read_status(statement, status_column);
    if (!status) {
        return std::unexpected{status.error()};
    }
    core::RecordedValue<double> result{};
    result.status = *status;
    if (*status == core::RecordedValueStatus::available) {
        if (sqlite3_column_type(statement, value_column) == SQLITE_NULL) {
            return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                 "available double is NULL")};
        }
        result.value = sqlite3_column_double(statement, value_column);
    }
    return result;
}

[[nodiscard]] std::expected<core::RecordedValue<std::uint64_t>, StorageError>
read_unsigned_value(sqlite3_stmt* statement, const int status_column,
                    const int value_column, const std::string_view field) {
    auto status = read_status(statement, status_column);
    if (!status) {
        return std::unexpected{status.error()};
    }
    core::RecordedValue<std::uint64_t> result{};
    result.status = *status;
    if (*status == core::RecordedValueStatus::available) {
        auto value = read_unsigned(statement, value_column, field);
        if (!value) {
            return std::unexpected{value.error()};
        }
        result.value = *value;
    }
    return result;
}

[[nodiscard]] std::expected<core::RecordedValue<std::uint8_t>, StorageError>
read_byte_value(sqlite3_stmt* statement, const int status_column,
                const int value_column, const std::uint8_t maximum) {
    auto status = read_status(statement, status_column);
    if (!status) return std::unexpected{status.error()};
    core::RecordedValue<std::uint8_t> result{};
    result.status = *status;
    if (*status == core::RecordedValueStatus::available) {
        if (sqlite3_column_type(statement, value_column) == SQLITE_NULL) {
            return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                 "available byte is NULL")};
        }
        const auto value = sqlite3_column_int(statement, value_column);
        if (value < 0 || value > maximum) {
            return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                 "recorded byte is out of range")};
        }
        result.value = static_cast<std::uint8_t>(value);
    }
    return result;
}

} // namespace detail

using namespace detail;

std::filesystem::path default_archive_path() {
    if (const auto override_path = environment_value("BLACKBOX_ARCHIVE_PATH")) {
        return std::filesystem::path{*override_path};
    }
    if (const auto local_app_data = environment_value("LOCALAPPDATA")) {
        return std::filesystem::path{*local_app_data} / "BlackBox" / "incidents.sqlite3";
    }
    if (const auto data_home = environment_value("XDG_DATA_HOME")) {
        return std::filesystem::path{*data_home} / "blackbox" / "incidents.sqlite3";
    }
    if (const auto home = environment_value("HOME")) {
        return std::filesystem::path{*home} / ".local" / "share" / "blackbox" /
               "incidents.sqlite3";
    }
    return std::filesystem::current_path() / "blackbox-data" / "incidents.sqlite3";
}

SqliteIncidentArchive::SqliteIncidentArchive(ArchiveConfiguration configuration)
    : configuration_{std::move(configuration)}, native_{std::make_unique<NativeState>()} {}

SqliteIncidentArchive::~SqliteIncidentArchive() {
    close();
}

std::expected<void, StorageError> SqliteIncidentArchive::open() noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        if (native_->database != nullptr) {
            return {};
        }
        if (configuration_.path.empty()) {
            return std::unexpected{simple_error(StorageErrorCode::cannot_open,
                                                 "archive path is empty")};
        }
        if (configuration_.open_mode == ArchiveOpenMode::read_only &&
            configuration_.path == std::filesystem::path{":memory:"}) {
            return std::unexpected{simple_error(StorageErrorCode::cannot_open,
                                                 "an in-memory archive cannot be read-only")};
        }
        if (configuration_.path != std::filesystem::path{":memory:"}) {
            if (auto header = validate_existing_archive_header(configuration_.path);
                !header) {
                return header;
            }
        }
        if (configuration_.open_mode == ArchiveOpenMode::read_only) {
            const auto status = std::filesystem::symlink_status(configuration_.path);
            if (!std::filesystem::is_regular_file(status)) {
                return std::unexpected{simple_error(StorageErrorCode::cannot_open,
                                                     "read-only archive is not a regular file")};
            }
        } else if (configuration_.path != std::filesystem::path{":memory:"}) {
            const auto parent = configuration_.path.parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
        }

        sqlite3* database = nullptr;
        int result{};
        if (configuration_.open_mode == ArchiveOpenMode::read_only) {
            const auto native_utf8 = configuration_.path.u8string();
            const std::string path_utf8{native_utf8.begin(), native_utf8.end()};
            result = sqlite3_open_v2(path_utf8.c_str(), &database,
                                     SQLITE_OPEN_READONLY, nullptr);
        } else {
#if defined(_WIN32)
            const auto native_path = configuration_.path.native();
            result = sqlite3_open16(native_path.c_str(), &database);
#else
            result = sqlite3_open_v2(configuration_.path.string().c_str(), &database,
                                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
#endif
        }
        if (result != SQLITE_OK) {
            auto error = database_error(database, "open incident archive", result);
            if (database != nullptr) {
                sqlite3_close(database);
            }
            return std::unexpected{std::move(error)};
        }
        if (configuration_.open_mode == ArchiveOpenMode::read_only) {
            if (auto configured = configure_read_only_connection(database, configuration_);
                !configured) {
                auto error = configured.error();
                sqlite3_close(database);
                return std::unexpected{std::move(error)};
            }
            native_->database = database;
            return {};
        }
        if (auto configured = configure_connection(database, configuration_); !configured) {
            auto error = configured.error();
            sqlite3_close(database);
            return std::unexpected{std::move(error)};
        }
        if (auto initialized = initialize_schema(database); !initialized) {
            auto error = initialized.error();
            sqlite3_close(database);
            return std::unexpected{std::move(error)};
        }
        if (auto journal = configure_journal(database); !journal) {
            auto error = journal.error();
            sqlite3_close(database);
            return std::unexpected{std::move(error)};
        }
        if (auto limited = configure_size_limit(database, configuration_.maximum_bytes);
            !limited) {
            auto error = limited.error();
            sqlite3_close(database);
            return std::unexpected{std::move(error)};
        }
        native_->database = database;
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::cannot_open, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::cannot_open,
                                             "unknown archive open failure")};
    }
}

void SqliteIncidentArchive::close() noexcept {
    const std::scoped_lock lock{native_->mutex};
    if (native_->database != nullptr) {
        sqlite3_close(native_->database);
        native_->database = nullptr;
    }
}

bool SqliteIncidentArchive::is_open() const noexcept {
    const std::scoped_lock lock{native_->mutex};
    return native_->database != nullptr;
}

std::expected<std::int64_t, StorageError> SqliteIncidentArchive::store(
    const core::IncidentSnapshot& incident) noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        Transaction transaction{database};
        if (auto begun = transaction.begin(); !begun) {
            return std::unexpected{begun.error()};
        }

        auto incident_insert = prepare(database, R"sql(
INSERT INTO incidents(
 created_utc_ms, capture_sequence, event_monotonic_ns, requested_start_ns,
 requested_end_ns, actual_start_ns, actual_end_ns, trigger_count,
 system_recorder_epoch, process_recorder_epoch, system_sample_count,
 process_metadata_count, process_sample_count, manual_trigger_count,
 automatic_trigger_count, automatic_resource, automatic_observed_value,
 automatic_baseline_value, automatic_score, automatic_signal,
 event_recorder_epoch, system_event_count, export_key)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,randomblob(16))
)sql", "prepare incident insert");
        if (!incident_insert) {
            return std::unexpected{incident_insert.error()};
        }
        auto* statement = incident_insert->get();
        const auto created = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto& header = incident.header();
        sqlite3_bind_int64(statement, 1, created);
        bind_unsigned(statement, 2, header.window.sequence);
        sqlite3_bind_int64(statement, 3, monotonic_nanoseconds(header.window.event_time));
        sqlite3_bind_int64(statement, 4, monotonic_nanoseconds(header.window.requested_start));
        sqlite3_bind_int64(statement, 5, monotonic_nanoseconds(header.window.requested_end));
        sqlite3_bind_int64(statement, 6, monotonic_nanoseconds(header.actual_start));
        sqlite3_bind_int64(statement, 7, monotonic_nanoseconds(header.actual_end));
        sqlite3_bind_int64(statement, 8, header.window.trigger_count);
        bind_unsigned(statement, 9, header.system_recorder_epoch);
        bind_unsigned(statement, 10, header.process_recorder_epoch);
        sqlite3_bind_int64(statement, 11,
                           static_cast<sqlite3_int64>(incident.system_samples().size()));
        sqlite3_bind_int64(statement, 12,
                           static_cast<sqlite3_int64>(incident.process_metadata().size()));
        sqlite3_bind_int64(statement, 13,
                           static_cast<sqlite3_int64>(incident.process_samples().size()));
        sqlite3_bind_int64(statement, 14, header.window.manual_trigger_count);
        sqlite3_bind_int64(statement, 15, header.window.automatic_trigger_count);
        sqlite3_bind_int(statement, 16,
                         static_cast<int>(header.window.automatic_resource));
        sqlite3_bind_double(statement, 17, header.window.automatic_observed_value);
        sqlite3_bind_double(statement, 18, header.window.automatic_baseline_value);
        sqlite3_bind_double(statement, 19, header.window.automatic_score);
        sqlite3_bind_int(statement, 20,
                         static_cast<int>(header.window.automatic_signal));
        bind_unsigned(statement, 21, header.event_recorder_epoch);
        sqlite3_bind_int64(statement, 22,
            static_cast<sqlite3_int64>(incident.system_events().size()));
        if (auto inserted = expect_done(database, statement, "insert incident"); !inserted) {
            return std::unexpected{inserted.error()};
        }
        const auto incident_id = sqlite3_last_insert_rowid(database);

        auto classification_insert = prepare(database, R"sql(
INSERT INTO incident_classification_history(
 incident_id, changed_utc_ms, category, user_feedback, origin)
VALUES(?,?,?,?,?)
)sql", "prepare initial incident classification");
        if (!classification_insert) {
            return std::unexpected{classification_insert.error()};
        }
        sqlite3_bind_int64(classification_insert->get(), 1, incident_id);
        sqlite3_bind_int64(classification_insert->get(), 2, created);
        sqlite3_bind_int(classification_insert->get(), 3,
                         static_cast<int>(IncidentCategory::unknown));
        sqlite3_bind_int(classification_insert->get(), 4,
                         static_cast<int>(IncidentUserFeedback::unanswered));
        sqlite3_bind_int(classification_insert->get(), 5,
                         static_cast<int>(ClassificationChangeOrigin::capture));
        if (auto inserted = expect_done(database, classification_insert->get(),
                                        "insert initial incident classification"); !inserted) {
            return std::unexpected{inserted.error()};
        }

        auto system_insert = prepare(database, R"sql(
INSERT INTO system_samples VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
)sql", "prepare system sample insert");
        if (!system_insert) {
            return std::unexpected{system_insert.error()};
        }
        auto quality_insert = prepare(database, R"sql(
INSERT INTO system_quality_samples VALUES(
 ?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
)sql", "prepare system quality sample insert");
        if (!quality_insert) {
            return std::unexpected{quality_insert.error()};
        }
        auto extended_insert = prepare(database, R"sql(
INSERT INTO system_extended_samples VALUES(
 ?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
)sql", "prepare system extended sample insert");
        if (!extended_insert) return std::unexpected{extended_insert.error()};
        std::size_t sample_index = 0U;
        for (const auto& sample : incident.system_samples()) {
            const auto current_sample_index = sample_index++;
            statement = system_insert->get();
            sqlite3_bind_int64(statement, 1, incident_id);
            sqlite3_bind_int64(statement, 2,
                               static_cast<sqlite3_int64>(current_sample_index));
            sqlite3_bind_int64(statement, 3, monotonic_nanoseconds(sample.observed_at));
            bind_double_value(statement, 4, 5, sample.cpu_fraction);
            bind_unsigned_value(statement, 6, 7, sample.memory_used_bytes);
            bind_unsigned_value(statement, 8, 9, sample.memory_total_bytes);
            bind_double_value(statement, 10, 11, sample.memory_fraction);
            bind_double_value(statement, 12, 13, sample.disk_read_bytes_per_second);
            bind_double_value(statement, 14, 15, sample.disk_write_bytes_per_second);
            bind_double_value(statement, 16, 17, sample.network_receive_bytes_per_second);
            bind_double_value(statement, 18, 19, sample.network_transmit_bytes_per_second);
            if (auto inserted = expect_done(database, statement, "insert system sample");
                !inserted) {
                return std::unexpected{inserted.error()};
            }

            statement = quality_insert->get();
            sqlite3_bind_int64(statement, 1, incident_id);
            sqlite3_bind_int64(statement, 2,
                               static_cast<sqlite3_int64>(current_sample_index));
            bind_double_value(statement, 3, 4, sample.disk_read_latency_seconds);
            bind_double_value(statement, 5, 6, sample.disk_write_latency_seconds);
            bind_double_value(statement, 7, 8, sample.disk_service_time_seconds);
            bind_double_value(statement, 9, 10, sample.disk_queue_depth);
            bind_unsigned_value(statement, 11, 12, sample.disk_worst_device_id);
            bind_byte_value(statement, 13, 14, sample.network_connectivity_level);
            bind_unsigned_value(statement, 15, 16,
                                sample.network_active_interfaces);
            bind_unsigned_value(statement, 17, 18,
                                sample.network_interface_changes);
            bind_double_value(statement, 19, 20,
                              sample.network_tcp_retransmit_fraction);
            bind_unsigned_value(statement, 21, 22,
                                sample.network_tcp_failed_connections);
            bind_unsigned_value(statement, 23, 24, sample.network_tcp_resets);
            if (auto inserted = expect_done(database, statement,
                                            "insert system quality sample");
                !inserted) {
                return std::unexpected{inserted.error()};
            }

            statement = extended_insert->get();
            sqlite3_bind_int64(statement, 1, incident_id);
            sqlite3_bind_int64(statement, 2,
                               static_cast<sqlite3_int64>(current_sample_index));
            bind_double_value(statement, 3, 4, sample.gpu_fraction);
            bind_unsigned_value(statement, 5, 6,
                                sample.gpu_dedicated_memory_bytes);
            bind_unsigned_value(statement, 7, 8,
                                sample.gpu_shared_memory_bytes);
            sqlite3_bind_int(statement, 9, stored_status(sample.foreground_process.status));
            if (sample.foreground_process.status == core::RecordedValueStatus::available) {
                sqlite3_bind_int64(statement, 10, sample.foreground_process.value.pid);
                bind_unsigned(statement, 11,
                              sample.foreground_process.value.creation_token);
            } else {
                sqlite3_bind_null(statement, 10);
                sqlite3_bind_null(statement, 11);
            }
            bind_double_value(statement, 12, 13, sample.foreground_gpu_fraction);
            bind_double_value(statement, 14, 15, sample.dpc_fraction);
            bind_double_value(statement, 16, 17, sample.interrupt_fraction);
            bind_double_value(statement, 18, 19, sample.dpc_rate);
            bind_double_value(statement, 20, 21, sample.cpu_current_mhz);
            bind_double_value(statement, 22, 23, sample.cpu_max_mhz);
            bind_double_value(statement, 24, 25, sample.cpu_thermal_limit_mhz);
            bind_double_value(statement, 26, 27,
                              sample.cpu_thermal_limit_fraction);
            bind_byte_value(statement, 28, 29, sample.power_source);
            bind_double_value(statement, 30, 31, sample.battery_fraction);
            sqlite3_bind_int(statement, 32,
                             stored_status(sample.battery_saver.status));
            if (sample.battery_saver.status == core::RecordedValueStatus::available) {
                sqlite3_bind_int(statement, 33, sample.battery_saver.value ? 1 : 0);
            } else {
                sqlite3_bind_null(statement, 33);
            }
            bind_double_value(statement, 34, 35, sample.system_uptime_seconds);
            if (auto inserted = expect_done(database, statement,
                                            "insert system extended sample");
                !inserted) {
                return std::unexpected{inserted.error()};
            }
        }

        std::vector<core::IncidentProcessIdentity> identities;
        identities.reserve(incident.process_metadata().size() +
                           incident.process_samples().size());
        for (const auto& info : incident.process_metadata()) {
            identities.push_back(info.identity);
        }
        for (const auto& sample : incident.process_samples()) {
            identities.push_back(sample.identity);
        }
        for (const auto& event : incident.system_events()) {
            if (event.has_process_identity) {
                identities.push_back(
                    {event.process_pid, event.process_creation_token});
            }
        }

        auto event_insert = prepare(database, R"sql(
INSERT INTO system_events VALUES(?,?,?,?,?,?,?,?,?,?,?,?)
)sql", "prepare system event insert");
        if (!event_insert) return std::unexpected{event_insert.error()};
        std::size_t event_index{};
        for (const auto& event : incident.system_events()) {
            statement = event_insert->get();
            sqlite3_bind_int64(statement, 1, incident_id);
            sqlite3_bind_int64(statement, 2,
                               static_cast<sqlite3_int64>(event_index++));
            sqlite3_bind_int64(statement, 3,
                               monotonic_nanoseconds(event.observed_at));
            if (event.has_source_utc_time) {
                sqlite3_bind_int64(statement, 4, event.source_utc_milliseconds);
            } else {
                sqlite3_bind_null(statement, 4);
            }
            sqlite3_bind_int(statement, 5, static_cast<int>(event.source));
            sqlite3_bind_int(statement, 6, static_cast<int>(event.kind));
            sqlite3_bind_int(statement, 7, static_cast<int>(event.level));
            sqlite3_bind_int64(statement, 8, event.native_event_id);
            sqlite3_bind_int64(statement, 9, event.detail);
            sqlite3_bind_int(statement, 10,
                             event.has_process_identity ? 1 : 0);
            if (event.has_process_identity) {
                sqlite3_bind_int64(statement, 11, event.process_pid);
                bind_unsigned(statement, 12, event.process_creation_token);
            } else {
                sqlite3_bind_null(statement, 11);
                sqlite3_bind_null(statement, 12);
            }
            if (auto inserted = expect_done(database, statement,
                                            "insert system event"); !inserted) {
                return std::unexpected{inserted.error()};
            }
        }
        std::sort(identities.begin(), identities.end());
        identities.erase(std::unique(identities.begin(), identities.end()), identities.end());

        auto identity_insert = prepare(database, R"sql(
INSERT INTO process_identities VALUES(?,?,?,?,?,?,?,?,?,?)
)sql", "prepare process identity insert");
        if (!identity_insert) {
            return std::unexpected{identity_insert.error()};
        }
        for (const auto identity : identities) {
            statement = identity_insert->get();
            sqlite3_bind_int64(statement, 1, incident_id);
            sqlite3_bind_int64(statement, 2, identity.pid);
            bind_unsigned(statement, 3, identity.creation_token);
            const auto metadata = std::find_if(
                incident.process_metadata().begin(), incident.process_metadata().end(),
                [identity](const core::IncidentProcessInfo& info) {
                    return info.identity == identity;
                });
            if (metadata == incident.process_metadata().end()) {
                sqlite3_bind_int(statement, 4, 0);
                sqlite3_bind_int(statement, 5,
                                 stored_status(core::RecordedValueStatus::unsupported));
                sqlite3_bind_null(statement, 6);
                sqlite3_bind_int(statement, 7,
                                 stored_status(core::RecordedValueStatus::unsupported));
                sqlite3_bind_null(statement, 8);
                sqlite3_bind_int(statement, 9,
                                 stored_status(core::RecordedValueStatus::unsupported));
                sqlite3_bind_null(statement, 10);
            } else {
                sqlite3_bind_int(statement, 4, 1);
                bind_pid_value(statement, 5, 6, metadata->parent_pid);
                bind_text_value(statement, 7, 8, metadata->name);
                bind_text_value(statement, 9, 10, metadata->executable_path);
            }
            if (auto inserted = expect_done(database, statement, "insert process identity");
                !inserted) {
                return std::unexpected{inserted.error()};
            }
        }

        auto process_insert = prepare(database, R"sql(
INSERT INTO process_samples VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)
)sql", "prepare process sample insert");
        if (!process_insert) {
            return std::unexpected{process_insert.error()};
        }
        sample_index = 0U;
        for (const auto& sample : incident.process_samples()) {
            statement = process_insert->get();
            sqlite3_bind_int64(statement, 1, incident_id);
            sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(sample_index++));
            sqlite3_bind_int64(statement, 3, monotonic_nanoseconds(sample.observed_at));
            sqlite3_bind_int64(statement, 4, sample.identity.pid);
            bind_unsigned(statement, 5, sample.identity.creation_token);
            bind_double_value(statement, 6, 7, sample.cpu_fraction);
            bind_unsigned_value(statement, 8, 9, sample.working_set_bytes);
            bind_double_value(statement, 10, 11, sample.disk_read_bytes_per_second);
            bind_double_value(statement, 12, 13, sample.disk_write_bytes_per_second);
            if (auto inserted = expect_done(database, statement, "insert process sample");
                !inserted) {
                return std::unexpected{inserted.error()};
            }
        }

        if (auto committed = transaction.commit(); !committed) {
            return std::unexpected{committed.error()};
        }
        return incident_id;
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown incident storage failure")};
    }
}

std::expected<std::vector<StoredIncidentSummary>, StorageError>
SqliteIncidentArchive::list(const std::size_t maximum_results) const noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        auto statement = prepare(database, R"sql(
SELECT id, created_utc_ms, capture_sequence, event_monotonic_ns,
       actual_start_ns, actual_end_ns, system_sample_count,
       process_metadata_count, process_sample_count, label, note, export_key
FROM incidents ORDER BY created_utc_ms DESC, id DESC LIMIT ?
)sql", "prepare incident discovery");
        if (!statement) {
            return std::unexpected{statement.error()};
        }
        sqlite3_bind_int64(statement->get(), 1,
                           static_cast<sqlite3_int64>(maximum_results));
        std::vector<StoredIncidentSummary> results;
        results.reserve(std::min<std::size_t>(maximum_results, 1'000U));
        while (true) {
            const auto result = sqlite3_step(statement->get());
            if (result == SQLITE_DONE) {
                break;
            }
            if (result != SQLITE_ROW) {
                return std::unexpected{database_error(database, "discover incidents", result)};
            }
            auto sequence = read_unsigned(statement->get(), 2, "capture_sequence");
            if (!sequence) {
                return std::unexpected{sequence.error()};
            }
            StoredIncidentSummary summary{};
            summary.id = sqlite3_column_int64(statement->get(), 0);
            summary.created_utc_milliseconds = sqlite3_column_int64(statement->get(), 1);
            summary.capture_sequence = *sequence;
            summary.event_monotonic_nanoseconds = sqlite3_column_int64(statement->get(), 3);
            summary.actual_start_nanoseconds = sqlite3_column_int64(statement->get(), 4);
            summary.actual_end_nanoseconds = sqlite3_column_int64(statement->get(), 5);
            summary.system_sample_count = static_cast<std::size_t>(
                sqlite3_column_int64(statement->get(), 6));
            summary.process_metadata_count = static_cast<std::size_t>(
                sqlite3_column_int64(statement->get(), 7));
            summary.process_sample_count = static_cast<std::size_t>(
                sqlite3_column_int64(statement->get(), 8));
            const auto* label = sqlite3_column_text(statement->get(), 9);
            const auto* note = sqlite3_column_text(statement->get(), 10);
            summary.label = label == nullptr ? "" : reinterpret_cast<const char*>(label);
            summary.note = note == nullptr ? "" : reinterpret_cast<const char*>(note);
            auto export_key = read_export_key(statement->get(), 11);
            if (!export_key) return std::unexpected{export_key.error()};
            summary.export_key = *export_key;
            results.push_back(std::move(summary));
        }
        return results;
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown incident discovery failure")};
    }
}

std::expected<StoredIncidentPage, StorageError> SqliteIncidentArchive::list_page(
    const IncidentListQuery& query) const noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        const auto limit = std::clamp<std::size_t>(query.limit, 1U,
                                                   maximum_incident_page_size);
        const char* ordering = "created_utc_ms DESC, id DESC";
        switch (query.sort) {
        case IncidentListSort::newest_first:
            break;
        case IncidentListSort::oldest_first:
            ordering = "created_utc_ms ASC, id ASC";
            break;
        case IncidentListSort::longest_first:
            ordering = "(actual_end_ns-actual_start_ns) DESC, id DESC";
            break;
        case IncidentListSort::shortest_first:
            ordering = "(actual_end_ns-actual_start_ns) ASC, id DESC";
            break;
        case IncidentListSort::label_ascending:
            ordering = "label COLLATE NOCASE ASC, created_utc_ms DESC, id DESC";
            break;
        case IncidentListSort::label_descending:
            ordering = "label COLLATE NOCASE DESC, created_utc_ms DESC, id DESC";
            break;
        }
        const std::string filter_sql = query.search.empty()
                                           ? ""
                                           : " WHERE instr(lower(label),lower(?))>0 OR "
                                             "instr(lower(note),lower(?))>0";
        const auto count_sql = "SELECT COUNT(*) FROM incidents" + filter_sql;
        auto count_statement = prepare(database, count_sql.c_str(),
                                       "prepare filtered incident count");
        if (!count_statement) return std::unexpected{count_statement.error()};
        if (!query.search.empty()) {
            sqlite3_bind_text(count_statement->get(), 1, query.search.c_str(),
                              static_cast<int>(query.search.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(count_statement->get(), 2, query.search.c_str(),
                              static_cast<int>(query.search.size()), SQLITE_TRANSIENT);
        }
        const auto count_result = sqlite3_step(count_statement->get());
        if (count_result != SQLITE_ROW) {
            return std::unexpected{database_error(database, "count filtered incidents",
                                                   count_result)};
        }

        const auto list_sql = std::string{
            "SELECT id,created_utc_ms,capture_sequence,event_monotonic_ns,"
            "actual_start_ns,actual_end_ns,system_sample_count,"
            "process_metadata_count,process_sample_count,label,note,export_key FROM incidents"} +
            filter_sql + " ORDER BY " + ordering + " LIMIT ? OFFSET ?";
        auto statement = prepare(database, list_sql.c_str(), "prepare incident page");
        if (!statement) return std::unexpected{statement.error()};
        auto parameter = 1;
        if (!query.search.empty()) {
            sqlite3_bind_text(statement->get(), parameter++, query.search.c_str(),
                              static_cast<int>(query.search.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(statement->get(), parameter++, query.search.c_str(),
                              static_cast<int>(query.search.size()), SQLITE_TRANSIENT);
        }
        sqlite3_bind_int64(statement->get(), parameter++, static_cast<sqlite3_int64>(limit));
        sqlite3_bind_int64(statement->get(), parameter,
                           static_cast<sqlite3_int64>(query.offset));

        StoredIncidentPage page{};
        page.total_matching = static_cast<std::uint64_t>(
            sqlite3_column_int64(count_statement->get(), 0));
        page.offset = query.offset;
        page.incidents.reserve(limit);
        while (true) {
            const auto result = sqlite3_step(statement->get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                return std::unexpected{database_error(database, "load incident page", result)};
            }
            auto sequence = read_unsigned(statement->get(), 2, "capture_sequence");
            if (!sequence) return std::unexpected{sequence.error()};
            StoredIncidentSummary summary{};
            summary.id = sqlite3_column_int64(statement->get(), 0);
            summary.created_utc_milliseconds = sqlite3_column_int64(statement->get(), 1);
            summary.capture_sequence = *sequence;
            summary.event_monotonic_nanoseconds = sqlite3_column_int64(statement->get(), 3);
            summary.actual_start_nanoseconds = sqlite3_column_int64(statement->get(), 4);
            summary.actual_end_nanoseconds = sqlite3_column_int64(statement->get(), 5);
            summary.system_sample_count = static_cast<std::size_t>(
                sqlite3_column_int64(statement->get(), 6));
            summary.process_metadata_count = static_cast<std::size_t>(
                sqlite3_column_int64(statement->get(), 7));
            summary.process_sample_count = static_cast<std::size_t>(
                sqlite3_column_int64(statement->get(), 8));
            const auto* label = sqlite3_column_text(statement->get(), 9);
            const auto* note = sqlite3_column_text(statement->get(), 10);
            summary.label = label == nullptr ? "" : reinterpret_cast<const char*>(label);
            summary.note = note == nullptr ? "" : reinterpret_cast<const char*>(note);
            auto export_key = read_export_key(statement->get(), 11);
            if (!export_key) return std::unexpected{export_key.error()};
            summary.export_key = *export_key;
            page.incidents.push_back(std::move(summary));
        }
        return page;
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown incident page failure")};
    }
}

std::expected<std::shared_ptr<const core::IncidentSnapshot>, StorageError>
SqliteIncidentArchive::load(const std::int64_t incident_id) const noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        auto incident_query = prepare(database, R"sql(
SELECT capture_sequence, event_monotonic_ns, requested_start_ns, requested_end_ns,
       actual_start_ns, actual_end_ns, trigger_count, system_recorder_epoch,
       process_recorder_epoch, manual_trigger_count, automatic_trigger_count,
       automatic_resource, automatic_observed_value, automatic_baseline_value,
        automatic_score, automatic_signal, event_recorder_epoch,
        system_event_count
FROM incidents WHERE id=?
)sql", "prepare incident load");
        if (!incident_query) {
            return std::unexpected{incident_query.error()};
        }
        sqlite3_bind_int64(incident_query->get(), 1, incident_id);
        const auto incident_result = sqlite3_step(incident_query->get());
        if (incident_result != SQLITE_ROW) {
            return std::unexpected{incident_result == SQLITE_DONE
                ? simple_error(StorageErrorCode::invalid_data, "incident not found")
                : database_error(database, "load incident", incident_result)};
        }
        auto sequence = read_unsigned(incident_query->get(), 0, "capture_sequence");
        auto system_epoch = read_unsigned(incident_query->get(), 7, "system_recorder_epoch");
        auto process_epoch = read_unsigned(incident_query->get(), 8, "process_recorder_epoch");
        auto event_epoch = read_unsigned(incident_query->get(), 16,
                                         "event_recorder_epoch");
        if (!sequence) return std::unexpected{sequence.error()};
        if (!system_epoch) return std::unexpected{system_epoch.error()};
        if (!process_epoch) return std::unexpected{process_epoch.error()};
        if (!event_epoch) return std::unexpected{event_epoch.error()};
        core::IncidentHeader header{};
        header.window.sequence = *sequence;
        header.window.event_time = monotonic_time(sqlite3_column_int64(incident_query->get(), 1));
        header.window.requested_start = monotonic_time(sqlite3_column_int64(incident_query->get(), 2));
        header.window.requested_end = monotonic_time(sqlite3_column_int64(incident_query->get(), 3));
        header.actual_start = monotonic_time(sqlite3_column_int64(incident_query->get(), 4));
        header.actual_end = monotonic_time(sqlite3_column_int64(incident_query->get(), 5));
        header.window.trigger_count = static_cast<std::uint32_t>(
            sqlite3_column_int64(incident_query->get(), 6));
        header.window.manual_trigger_count = static_cast<std::uint32_t>(
            sqlite3_column_int64(incident_query->get(), 9));
        header.window.automatic_trigger_count = static_cast<std::uint32_t>(
            sqlite3_column_int64(incident_query->get(), 10));
        header.window.automatic_resource = static_cast<core::AutomaticIncidentResource>(
            sqlite3_column_int(incident_query->get(), 11));
        header.window.automatic_observed_value =
            sqlite3_column_double(incident_query->get(), 12);
        header.window.automatic_baseline_value =
            sqlite3_column_double(incident_query->get(), 13);
        header.window.automatic_score = sqlite3_column_double(incident_query->get(), 14);
        header.window.automatic_signal = static_cast<core::AutomaticIncidentSignal>(
            sqlite3_column_int(incident_query->get(), 15));
        header.system_recorder_epoch = *system_epoch;
        header.process_recorder_epoch = *process_epoch;
        header.event_recorder_epoch = *event_epoch;

        std::vector<core::IncidentSystemSample> systems;
        auto system_query = prepare(database, R"sql(
SELECT observed_ns, cpu_status, cpu_fraction, memory_used_status, memory_used_bytes,
 memory_total_status, memory_total_bytes, memory_fraction_status, memory_fraction,
 disk_read_status, disk_read_bps, disk_write_status, disk_write_bps,
 network_receive_status, network_receive_bps, network_transmit_status,
 network_transmit_bps
FROM system_samples WHERE incident_id=? ORDER BY sample_index
)sql", "prepare system sample load");
        if (!system_query) return std::unexpected{system_query.error()};
        sqlite3_bind_int64(system_query->get(), 1, incident_id);
        while (true) {
            const auto result = sqlite3_step(system_query->get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                return std::unexpected{database_error(database, "load system samples", result)};
            }
            core::IncidentSystemSample sample{};
            sample.observed_at = monotonic_time(sqlite3_column_int64(system_query->get(), 0));
            auto cpu = read_double_value(system_query->get(), 1, 2);
            auto used = read_unsigned_value(system_query->get(), 3, 4, "memory_used_bytes");
            auto total = read_unsigned_value(system_query->get(), 5, 6, "memory_total_bytes");
            auto memory = read_double_value(system_query->get(), 7, 8);
            auto disk_read = read_double_value(system_query->get(), 9, 10);
            auto disk_write = read_double_value(system_query->get(), 11, 12);
            auto network_receive = read_double_value(system_query->get(), 13, 14);
            auto network_transmit = read_double_value(system_query->get(), 15, 16);
            if (!cpu) return std::unexpected{cpu.error()};
            if (!used) return std::unexpected{used.error()};
            if (!total) return std::unexpected{total.error()};
            if (!memory) return std::unexpected{memory.error()};
            if (!disk_read) return std::unexpected{disk_read.error()};
            if (!disk_write) return std::unexpected{disk_write.error()};
            if (!network_receive) return std::unexpected{network_receive.error()};
            if (!network_transmit) return std::unexpected{network_transmit.error()};
            sample.cpu_fraction = *cpu;
            sample.memory_used_bytes = *used;
            sample.memory_total_bytes = *total;
            sample.memory_fraction = *memory;
            sample.disk_read_bytes_per_second = *disk_read;
            sample.disk_write_bytes_per_second = *disk_write;
            sample.network_receive_bytes_per_second = *network_receive;
            sample.network_transmit_bytes_per_second = *network_transmit;
            systems.push_back(sample);
        }

        auto quality_query = prepare(database, R"sql(
SELECT sample_index,
 disk_read_latency_status, disk_read_latency_seconds,
 disk_write_latency_status, disk_write_latency_seconds,
 disk_service_time_status, disk_service_time_seconds,
 disk_queue_depth_status, disk_queue_depth,
 disk_device_status, disk_device_id,
 network_connectivity_status, network_connectivity_level,
 network_interfaces_status, network_active_interfaces,
 network_changes_status, network_interface_changes,
 tcp_retransmit_status, tcp_retransmit_fraction,
 tcp_failures_status, tcp_failed_connections,
 tcp_resets_status, tcp_resets
FROM system_quality_samples WHERE incident_id=? ORDER BY sample_index
)sql", "prepare system quality sample load");
        if (!quality_query) return std::unexpected{quality_query.error()};
        sqlite3_bind_int64(quality_query->get(), 1, incident_id);
        std::optional<std::size_t> previous_quality_index;
        while (true) {
            const auto result = sqlite3_step(quality_query->get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                return std::unexpected{database_error(
                    database, "load system quality samples", result)};
            }
            const auto signed_index = sqlite3_column_int64(quality_query->get(), 0);
            if (signed_index < 0 ||
                static_cast<std::uint64_t>(signed_index) >= systems.size()) {
                return std::unexpected{simple_error(
                    StorageErrorCode::invalid_data,
                    "system quality sample index is out of range")};
            }
            const auto index = static_cast<std::size_t>(signed_index);
            if (previous_quality_index.has_value() &&
                index <= *previous_quality_index) {
                return std::unexpected{simple_error(
                    StorageErrorCode::invalid_data,
                    "system quality sample indexes are not unique and ordered")};
            }
            previous_quality_index = index;

            auto read_latency = read_double_value(quality_query->get(), 1, 2);
            auto write_latency = read_double_value(quality_query->get(), 3, 4);
            auto service_time = read_double_value(quality_query->get(), 5, 6);
            auto queue_depth = read_double_value(quality_query->get(), 7, 8);
            auto device = read_unsigned_value(quality_query->get(), 9, 10,
                                              "disk_device_id");
            auto connectivity = read_byte_value(quality_query->get(), 11, 12, 4U);
            auto interfaces = read_unsigned_value(quality_query->get(), 13, 14,
                                                  "network_active_interfaces");
            auto changes = read_unsigned_value(quality_query->get(), 15, 16,
                                               "network_interface_changes");
            auto retransmit = read_double_value(quality_query->get(), 17, 18);
            auto failures = read_unsigned_value(quality_query->get(), 19, 20,
                                                "tcp_failed_connections");
            auto resets = read_unsigned_value(quality_query->get(), 21, 22,
                                              "tcp_resets");
            if (!read_latency) return std::unexpected{read_latency.error()};
            if (!write_latency) return std::unexpected{write_latency.error()};
            if (!service_time) return std::unexpected{service_time.error()};
            if (!queue_depth) return std::unexpected{queue_depth.error()};
            if (!device) return std::unexpected{device.error()};
            if (!connectivity) return std::unexpected{connectivity.error()};
            if (!interfaces) return std::unexpected{interfaces.error()};
            if (!changes) return std::unexpected{changes.error()};
            if (!retransmit) return std::unexpected{retransmit.error()};
            if (!failures) return std::unexpected{failures.error()};
            if (!resets) return std::unexpected{resets.error()};

            auto& sample = systems[index];
            sample.disk_read_latency_seconds = *read_latency;
            sample.disk_write_latency_seconds = *write_latency;
            sample.disk_service_time_seconds = *service_time;
            sample.disk_queue_depth = *queue_depth;
            sample.disk_worst_device_id = *device;
            sample.network_connectivity_level = *connectivity;
            sample.network_active_interfaces = *interfaces;
            sample.network_interface_changes = *changes;
            sample.network_tcp_retransmit_fraction = *retransmit;
            sample.network_tcp_failed_connections = *failures;
            sample.network_tcp_resets = *resets;
        }

        auto extended_query = prepare(database, R"sql(
SELECT sample_index,
 gpu_status, gpu_fraction, gpu_dedicated_status, gpu_dedicated_bytes,
 gpu_shared_status, gpu_shared_bytes, foreground_status, foreground_pid,
 foreground_creation_token, foreground_gpu_status, foreground_gpu_fraction,
 dpc_status, dpc_fraction, interrupt_status, interrupt_fraction,
 dpc_rate_status, dpc_rate, cpu_current_status, cpu_current_mhz,
 cpu_max_status, cpu_max_mhz, cpu_limit_status, cpu_limit_mhz,
 cpu_limit_fraction_status, cpu_limit_fraction, power_source_status, power_source,
 battery_status, battery_fraction, battery_saver_status, battery_saver,
 uptime_status, uptime_seconds
FROM system_extended_samples WHERE incident_id=? ORDER BY sample_index
)sql", "prepare system extended sample load");
        if (!extended_query) return std::unexpected{extended_query.error()};
        sqlite3_bind_int64(extended_query->get(), 1, incident_id);
        std::optional<std::size_t> previous_extended_index;
        while (true) {
            const auto result = sqlite3_step(extended_query->get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                return std::unexpected{database_error(
                    database, "load system extended samples", result)};
            }
            const auto signed_index = sqlite3_column_int64(extended_query->get(), 0);
            if (signed_index < 0 ||
                static_cast<std::uint64_t>(signed_index) >= systems.size()) {
                return std::unexpected{simple_error(
                    StorageErrorCode::invalid_data,
                    "system extended sample index is out of range")};
            }
            const auto index = static_cast<std::size_t>(signed_index);
            if (previous_extended_index.has_value() &&
                index <= *previous_extended_index) {
                return std::unexpected{simple_error(
                    StorageErrorCode::invalid_data,
                    "system extended sample indexes are not unique and ordered")};
            }
            previous_extended_index = index;
            auto& sample = systems[index];
            auto gpu = read_double_value(extended_query->get(), 1, 2);
            auto dedicated = read_unsigned_value(extended_query->get(), 3, 4,
                                                 "gpu_dedicated_bytes");
            auto shared = read_unsigned_value(extended_query->get(), 5, 6,
                                              "gpu_shared_bytes");
            auto foreground_status = read_status(extended_query->get(), 7);
            auto foreground_gpu = read_double_value(extended_query->get(), 10, 11);
            auto dpc = read_double_value(extended_query->get(), 12, 13);
            auto interrupt = read_double_value(extended_query->get(), 14, 15);
            auto dpc_rate = read_double_value(extended_query->get(), 16, 17);
            auto current = read_double_value(extended_query->get(), 18, 19);
            auto maximum = read_double_value(extended_query->get(), 20, 21);
            auto limit = read_double_value(extended_query->get(), 22, 23);
            auto limit_fraction = read_double_value(extended_query->get(), 24, 25);
            auto power = read_byte_value(extended_query->get(), 26, 27, 3U);
            auto battery = read_double_value(extended_query->get(), 28, 29);
            auto saver_status = read_status(extended_query->get(), 30);
            auto uptime = read_double_value(extended_query->get(), 32, 33);
            if (!gpu) return std::unexpected{gpu.error()};
            if (!dedicated) return std::unexpected{dedicated.error()};
            if (!shared) return std::unexpected{shared.error()};
            if (!foreground_status) return std::unexpected{foreground_status.error()};
            if (!foreground_gpu) return std::unexpected{foreground_gpu.error()};
            if (!dpc) return std::unexpected{dpc.error()};
            if (!interrupt) return std::unexpected{interrupt.error()};
            if (!dpc_rate) return std::unexpected{dpc_rate.error()};
            if (!current) return std::unexpected{current.error()};
            if (!maximum) return std::unexpected{maximum.error()};
            if (!limit) return std::unexpected{limit.error()};
            if (!limit_fraction) return std::unexpected{limit_fraction.error()};
            if (!power) return std::unexpected{power.error()};
            if (!battery) return std::unexpected{battery.error()};
            if (!saver_status) return std::unexpected{saver_status.error()};
            if (!uptime) return std::unexpected{uptime.error()};
            sample.gpu_fraction = *gpu;
            sample.gpu_dedicated_memory_bytes = *dedicated;
            sample.gpu_shared_memory_bytes = *shared;
            sample.foreground_process.status = *foreground_status;
            if (*foreground_status == core::RecordedValueStatus::available) {
                const auto pid = sqlite3_column_int64(extended_query->get(), 8);
                auto token = read_unsigned(extended_query->get(), 9,
                                           "foreground_creation_token");
                if (!token || pid <= 0 || pid > UINT32_MAX) {
                    return std::unexpected{simple_error(
                        StorageErrorCode::invalid_data,
                        "foreground process identity is invalid")};
                }
                sample.foreground_process.value = {
                    static_cast<std::uint32_t>(pid), *token};
            }
            sample.foreground_gpu_fraction = *foreground_gpu;
            sample.dpc_fraction = *dpc;
            sample.interrupt_fraction = *interrupt;
            sample.dpc_rate = *dpc_rate;
            sample.cpu_current_mhz = *current;
            sample.cpu_max_mhz = *maximum;
            sample.cpu_thermal_limit_mhz = *limit;
            sample.cpu_thermal_limit_fraction = *limit_fraction;
            sample.power_source = *power;
            sample.battery_fraction = *battery;
            sample.battery_saver.status = *saver_status;
            if (*saver_status == core::RecordedValueStatus::available) {
                const auto value = sqlite3_column_int(extended_query->get(), 31);
                if (value != 0 && value != 1) {
                    return std::unexpected{simple_error(
                        StorageErrorCode::invalid_data,
                        "battery saver value is invalid")};
                }
                sample.battery_saver.value = value != 0;
            }
            sample.system_uptime_seconds = *uptime;
        }

        std::vector<core::SystemEvent> events;
        auto event_query = prepare(database, R"sql(
SELECT observed_ns, source_utc_ms, source, kind, level, native_event_id, detail,
       has_process_identity, process_pid, process_creation_token
FROM system_events WHERE incident_id=? ORDER BY event_index
)sql", "prepare system event load");
        if (!event_query) return std::unexpected{event_query.error()};
        sqlite3_bind_int64(event_query->get(), 1, incident_id);
        while (true) {
            const auto result = sqlite3_step(event_query->get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                return std::unexpected{database_error(database,
                    "load system events", result)};
            }
            const auto source = sqlite3_column_int(event_query->get(), 2);
            const auto kind = sqlite3_column_int(event_query->get(), 3);
            const auto level = sqlite3_column_int(event_query->get(), 4);
            if (source < 0 || source > 10 || kind < 0 ||
                kind > static_cast<int>(core::SystemEventKind::process_exited) ||
                level < 0 || level > 2) {
                return std::unexpected{simple_error(
                    StorageErrorCode::invalid_data, "system event enum is invalid")};
            }
            core::SystemEvent event{};
            event.observed_at = monotonic_time(
                sqlite3_column_int64(event_query->get(), 0));
            event.has_source_utc_time =
                sqlite3_column_type(event_query->get(), 1) != SQLITE_NULL;
            if (event.has_source_utc_time) {
                event.source_utc_milliseconds =
                    sqlite3_column_int64(event_query->get(), 1);
            }
            event.source = static_cast<core::SystemEventSource>(source);
            event.kind = static_cast<core::SystemEventKind>(kind);
            event.level = static_cast<core::SystemEventLevel>(level);
            event.native_event_id = static_cast<std::uint32_t>(
                sqlite3_column_int64(event_query->get(), 5));
            event.detail = static_cast<std::uint32_t>(
                sqlite3_column_int64(event_query->get(), 6));
            const auto identity_present = sqlite3_column_int(
                event_query->get(), 7);
            const bool process_event =
                source == static_cast<int>(core::SystemEventSource::process) &&
                (kind == static_cast<int>(core::SystemEventKind::process_started) ||
                 kind == static_cast<int>(core::SystemEventKind::process_exited));
            if ((identity_present != 0 && identity_present != 1) ||
                (identity_present == 1) != process_event) {
                return std::unexpected{simple_error(
                    StorageErrorCode::invalid_data,
                    "system event process identity flag is invalid")};
            }
            event.has_process_identity = identity_present == 1;
            if (event.has_process_identity) {
                const auto pid = sqlite3_column_int64(event_query->get(), 8);
                auto token = read_unsigned(event_query->get(), 9,
                                           "event_process_creation_token");
                if (!token || *token == 0U || pid <= 0 || pid > UINT32_MAX) {
                    return std::unexpected{simple_error(
                        StorageErrorCode::invalid_data,
                        "system event process identity is invalid")};
                }
                event.process_pid = static_cast<std::uint32_t>(pid);
                event.process_creation_token = *token;
            } else if (sqlite3_column_type(event_query->get(), 8) != SQLITE_NULL ||
                       sqlite3_column_type(event_query->get(), 9) != SQLITE_NULL) {
                return std::unexpected{simple_error(
                    StorageErrorCode::invalid_data,
                    "system event has unexpected process identity data")};
            }
            events.push_back(event);
        }

        std::vector<core::IncidentProcessInfo> metadata;
        auto metadata_query = prepare(database, R"sql(
SELECT pid, creation_token, parent_pid_status, parent_pid, name_status, name,
       path_status, executable_path
FROM process_identities
WHERE incident_id=? AND metadata_present=1
ORDER BY pid, creation_token
)sql", "prepare process metadata load");
        if (!metadata_query) return std::unexpected{metadata_query.error()};
        sqlite3_bind_int64(metadata_query->get(), 1, incident_id);
        while (true) {
            const auto result = sqlite3_step(metadata_query->get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                return std::unexpected{database_error(database, "load process metadata", result)};
            }
            core::IncidentProcessInfo info{};
            info.identity.pid = static_cast<std::uint32_t>(
                sqlite3_column_int64(metadata_query->get(), 0));
            auto token = read_unsigned(metadata_query->get(), 1, "creation_token");
            auto parent_status = read_status(metadata_query->get(), 2);
            auto name_status = read_status(metadata_query->get(), 4);
            auto path_status = read_status(metadata_query->get(), 6);
            if (!token) return std::unexpected{token.error()};
            if (!parent_status) return std::unexpected{parent_status.error()};
            if (!name_status) return std::unexpected{name_status.error()};
            if (!path_status) return std::unexpected{path_status.error()};
            info.identity.creation_token = *token;
            info.parent_pid.status = *parent_status;
            if (*parent_status == core::RecordedValueStatus::available) {
                info.parent_pid.value = static_cast<std::uint32_t>(
                    sqlite3_column_int64(metadata_query->get(), 3));
            }
            const auto read_text = [statement = metadata_query->get()](
                                       const int column,
                                       core::RecordedValue<std::string>& destination) {
                if (destination.status != core::RecordedValueStatus::available) return;
                const auto* text = reinterpret_cast<const char*>(
                    sqlite3_column_text(statement, column));
                const auto bytes = sqlite3_column_bytes(statement, column);
                if (text != nullptr) destination.value.assign(text, static_cast<std::size_t>(bytes));
            };
            info.name.status = *name_status;
            read_text(5, info.name);
            info.executable_path.status = *path_status;
            read_text(7, info.executable_path);
            metadata.push_back(std::move(info));
        }

        std::vector<core::IncidentProcessSample> processes;
        auto process_query = prepare(database, R"sql(
SELECT observed_ns, pid, creation_token, cpu_status, cpu_fraction,
 working_set_status, working_set_bytes, disk_read_status, disk_read_bps,
 disk_write_status, disk_write_bps
FROM process_samples WHERE incident_id=? ORDER BY sample_index
)sql", "prepare process sample load");
        if (!process_query) return std::unexpected{process_query.error()};
        sqlite3_bind_int64(process_query->get(), 1, incident_id);
        while (true) {
            const auto result = sqlite3_step(process_query->get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                return std::unexpected{database_error(database, "load process samples", result)};
            }
            core::IncidentProcessSample sample{};
            sample.observed_at = monotonic_time(sqlite3_column_int64(process_query->get(), 0));
            sample.identity.pid = static_cast<std::uint32_t>(
                sqlite3_column_int64(process_query->get(), 1));
            auto token = read_unsigned(process_query->get(), 2, "creation_token");
            auto cpu = read_double_value(process_query->get(), 3, 4);
            auto working = read_unsigned_value(process_query->get(), 5, 6,
                                               "working_set_bytes");
            auto read = read_double_value(process_query->get(), 7, 8);
            auto write = read_double_value(process_query->get(), 9, 10);
            if (!token) return std::unexpected{token.error()};
            if (!cpu) return std::unexpected{cpu.error()};
            if (!working) return std::unexpected{working.error()};
            if (!read) return std::unexpected{read.error()};
            if (!write) return std::unexpected{write.error()};
            sample.identity.creation_token = *token;
            sample.cpu_fraction = *cpu;
            sample.working_set_bytes = *working;
            sample.disk_read_bytes_per_second = *read;
            sample.disk_write_bytes_per_second = *write;
            processes.push_back(sample);
        }
        return std::make_shared<const core::IncidentSnapshot>(
            std::move(header), std::move(systems), std::move(metadata),
            std::move(processes), std::move(events));
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown incident load failure")};
    }
}

std::expected<IncidentAnnotation, StorageError> SqliteIncidentArchive::annotation(
    const std::int64_t incident_id) const noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        auto statement = prepare(database, "SELECT label,note,user_feedback,category FROM incidents WHERE id=?",
                                 "prepare incident annotation load");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_int64(statement->get(), 1, incident_id);
        const auto result = sqlite3_step(statement->get());
        if (result != SQLITE_ROW) {
            return std::unexpected{result == SQLITE_DONE
                ? simple_error(StorageErrorCode::invalid_data, "incident not found")
                : database_error(database, "load incident annotation", result)};
        }
        const auto* label = sqlite3_column_text(statement->get(), 0);
        const auto* note = sqlite3_column_text(statement->get(), 1);
        const auto feedback = sqlite3_column_int(statement->get(), 2);
        const auto category = sqlite3_column_int(statement->get(), 3);
        if (feedback < static_cast<int>(IncidentUserFeedback::unanswered) ||
            feedback > static_cast<int>(IncidentUserFeedback::did_not_notice_problem) ||
            category < static_cast<int>(IncidentCategory::unknown) ||
            category > static_cast<int>(IncidentCategory::audio)) {
            return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                 "invalid incident classification")};
        }
        return IncidentAnnotation{
            label == nullptr ? "" : reinterpret_cast<const char*>(label),
            note == nullptr ? "" : reinterpret_cast<const char*>(note),
            static_cast<IncidentUserFeedback>(feedback),
            static_cast<IncidentCategory>(category)};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown annotation load failure")};
    }
}

std::expected<void, StorageError> SqliteIncidentArchive::update_annotation(
    const std::int64_t incident_id, const IncidentAnnotation& annotation_value) noexcept {
    return update_annotation_with_origin(incident_id, annotation_value,
                                         ClassificationChangeOrigin::user);
}

std::expected<void, StorageError> SqliteIncidentArchive::update_annotation_with_origin(
    const std::int64_t incident_id, const IncidentAnnotation& annotation_value,
    const ClassificationChangeOrigin origin) noexcept {
    try {
        if (annotation_value.label.size() > maximum_incident_label_bytes ||
            annotation_value.note.size() > maximum_incident_note_bytes ||
            annotation_value.user_feedback > IncidentUserFeedback::did_not_notice_problem ||
            annotation_value.category > IncidentCategory::audio ||
            origin > ClassificationChangeOrigin::dataset_import) {
            return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                 "invalid incident annotation")};
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        Transaction transaction{database};
        if (auto begun = transaction.begin(); !begun) {
            return std::unexpected{begun.error()};
        }
        auto previous = prepare(database,
                                "SELECT category,user_feedback FROM incidents WHERE id=?",
                                "prepare previous incident classification");
        if (!previous) return std::unexpected{previous.error()};
        sqlite3_bind_int64(previous->get(), 1, incident_id);
        const auto previous_result = sqlite3_step(previous->get());
        if (previous_result != SQLITE_ROW) {
            return std::unexpected{previous_result == SQLITE_DONE
                ? simple_error(StorageErrorCode::invalid_data, "incident not found")
                : database_error(database, "load previous incident classification",
                                 previous_result)};
        }
        const auto previous_category = sqlite3_column_int(previous->get(), 0);
        const auto previous_feedback = sqlite3_column_int(previous->get(), 1);
        const auto classification_changed =
            previous_category != static_cast<int>(annotation_value.category) ||
            previous_feedback != static_cast<int>(annotation_value.user_feedback);
        auto statement = prepare(database,
                                 "UPDATE incidents SET label=?,note=?,user_feedback=?,category=? WHERE id=?",
                                 "prepare incident annotation update");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_text(statement->get(), 1, annotation_value.label.c_str(),
                          static_cast<int>(annotation_value.label.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(statement->get(), 2, annotation_value.note.c_str(),
                          static_cast<int>(annotation_value.note.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(statement->get(), 3,
                         static_cast<int>(annotation_value.user_feedback));
        sqlite3_bind_int(statement->get(), 4,
                         static_cast<int>(annotation_value.category));
        sqlite3_bind_int64(statement->get(), 5, incident_id);
        if (auto updated = expect_done(database, statement->get(),
                                       "update incident annotation"); !updated) {
            return std::unexpected{updated.error()};
        }
        if (sqlite3_changes(database) != 1) {
            return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                 "incident not found")};
        }
        if (classification_changed) {
            auto history_insert = prepare(database, R"sql(
INSERT INTO incident_classification_history(
 incident_id, changed_utc_ms, category, user_feedback, origin)
VALUES(?,?,?,?,?)
)sql", "prepare incident classification history insert");
            if (!history_insert) return std::unexpected{history_insert.error()};
            const auto changed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            sqlite3_bind_int64(history_insert->get(), 1, incident_id);
            sqlite3_bind_int64(history_insert->get(), 2, changed);
            sqlite3_bind_int(history_insert->get(), 3,
                             static_cast<int>(annotation_value.category));
            sqlite3_bind_int(history_insert->get(), 4,
                             static_cast<int>(annotation_value.user_feedback));
            sqlite3_bind_int(history_insert->get(), 5, static_cast<int>(origin));
            if (auto inserted = expect_done(database, history_insert->get(),
                                            "insert incident classification history");
                !inserted) {
                return std::unexpected{inserted.error()};
            }
            auto prune = prepare(database, R"sql(
DELETE FROM incident_classification_history
WHERE incident_id=? AND event_id NOT IN (
 SELECT event_id FROM incident_classification_history
 WHERE incident_id=? ORDER BY changed_utc_ms DESC,event_id DESC LIMIT ?
)
)sql", "prepare incident classification history prune");
            if (!prune) return std::unexpected{prune.error()};
            sqlite3_bind_int64(prune->get(), 1, incident_id);
            sqlite3_bind_int64(prune->get(), 2, incident_id);
            sqlite3_bind_int64(prune->get(), 3,
                               static_cast<sqlite3_int64>(
                                   maximum_incident_classification_history));
            if (auto pruned = expect_done(database, prune->get(),
                                          "prune incident classification history"); !pruned) {
                return std::unexpected{pruned.error()};
            }
        }
        return transaction.commit();
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown annotation update failure")};
    }
}

std::expected<std::vector<IncidentClassificationHistoryEntry>, StorageError>
SqliteIncidentArchive::classification_history(const std::int64_t incident_id) const noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        auto statement = prepare(database, R"sql(
SELECT changed_utc_ms,category,user_feedback,origin
FROM incident_classification_history WHERE incident_id=?
ORDER BY changed_utc_ms,event_id
)sql", "prepare incident classification history load");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_int64(statement->get(), 1, incident_id);
        std::vector<IncidentClassificationHistoryEntry> history;
        history.reserve(maximum_incident_classification_history);
        while (true) {
            const auto result = sqlite3_step(statement->get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                return std::unexpected{database_error(database,
                    "load incident classification history", result)};
            }
            const auto category = sqlite3_column_int(statement->get(), 1);
            const auto feedback = sqlite3_column_int(statement->get(), 2);
            const auto origin = sqlite3_column_int(statement->get(), 3);
            if (category < 0 || category > static_cast<int>(IncidentCategory::audio) ||
                feedback < 0 ||
                feedback > static_cast<int>(IncidentUserFeedback::did_not_notice_problem) ||
                origin < 0 ||
                origin > static_cast<int>(ClassificationChangeOrigin::dataset_import)) {
                return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                     "invalid classification history")};
            }
            history.push_back(IncidentClassificationHistoryEntry{
                sqlite3_column_int64(statement->get(), 0),
                static_cast<IncidentCategory>(category),
                static_cast<IncidentUserFeedback>(feedback),
                static_cast<ClassificationChangeOrigin>(origin)});
        }
        return history;
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown classification history failure")};
    }
}

std::expected<FeedbackCalibrationContext, StorageError>
SqliteIncidentArchive::feedback_calibration_context(
    const std::int64_t incident_id,
    const std::size_t maximum_observations) const noexcept {
    try {
        if (incident_id <= 0 || maximum_observations == 0U ||
            maximum_observations > maximum_feedback_calibration_observations) {
            return std::unexpected{simple_error(
                StorageErrorCode::invalid_data,
                "invalid feedback calibration query")};
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        auto current = prepare(database,
                               "SELECT created_utc_ms FROM incidents WHERE id=?",
                               "prepare feedback calibration incident load");
        if (!current) return std::unexpected{current.error()};
        sqlite3_bind_int64(current->get(), 1, incident_id);
        const auto current_result = sqlite3_step(current->get());
        if (current_result != SQLITE_ROW) {
            return std::unexpected{current_result == SQLITE_DONE
                ? simple_error(StorageErrorCode::invalid_data, "incident not found")
                : database_error(database, "load feedback calibration incident",
                                 current_result)};
        }
        FeedbackCalibrationContext context{};
        context.incident_id = incident_id;
        context.incident_utc_milliseconds = sqlite3_column_int64(current->get(), 0);

        auto profile = prepare(database, R"sql(
SELECT revision,reset_after_utc_ms,rollback_available
FROM feedback_profile_state WHERE singleton=1
)sql", "prepare feedback profile state load");
        if (!profile) return std::unexpected{profile.error()};
        const auto profile_result = sqlite3_step(profile->get());
        if (profile_result != SQLITE_ROW || sqlite3_column_int64(profile->get(), 0) < 0 ||
            sqlite3_column_int64(profile->get(), 1) < 0 ||
            (sqlite3_column_int(profile->get(), 2) != 0 &&
             sqlite3_column_int(profile->get(), 2) != 1)) {
            return std::unexpected{profile_result == SQLITE_ROW
                ? simple_error(StorageErrorCode::invalid_data,
                               "invalid feedback profile state")
                : database_error(database, "load feedback profile state",
                                 profile_result)};
        }
        context.profile_revision = static_cast<std::uint64_t>(
            sqlite3_column_int64(profile->get(), 0));
        context.reset_after_utc_milliseconds =
            sqlite3_column_int64(profile->get(), 1);
        context.rollback_available = sqlite3_column_int(profile->get(), 2) != 0;

        auto history = prepare(database, R"sql(
SELECT id,created_utc_ms,automatic_resource,automatic_signal,user_feedback
FROM incidents
WHERE automatic_trigger_count>0 AND user_feedback<>0
  AND created_utc_ms>?
  AND (created_utc_ms<? OR (created_utc_ms=? AND id<?))
ORDER BY created_utc_ms DESC,id DESC LIMIT ?
)sql", "prepare feedback calibration history load");
        if (!history) return std::unexpected{history.error()};
        sqlite3_bind_int64(history->get(), 1,
                           context.reset_after_utc_milliseconds);
        sqlite3_bind_int64(history->get(), 2, context.incident_utc_milliseconds);
        sqlite3_bind_int64(history->get(), 3, context.incident_utc_milliseconds);
        sqlite3_bind_int64(history->get(), 4, incident_id);
        sqlite3_bind_int64(history->get(), 5,
                           static_cast<sqlite3_int64>(maximum_observations));
        context.history.reserve(maximum_observations);
        while (true) {
            const auto result = sqlite3_step(history->get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                return std::unexpected{database_error(
                    database, "load feedback calibration history", result)};
            }
            const auto resource = sqlite3_column_int(history->get(), 2);
            const auto signal = sqlite3_column_int(history->get(), 3);
            const auto feedback = sqlite3_column_int(history->get(), 4);
            if (resource < static_cast<int>(core::AutomaticIncidentResource::none) ||
                resource > static_cast<int>(core::AutomaticIncidentResource::network) ||
                signal < static_cast<int>(
                    core::AutomaticIncidentSignal::throughput_or_utilization) ||
                signal > static_cast<int>(
                    core::AutomaticIncidentSignal::storage_io_retry) ||
                feedback < static_cast<int>(IncidentUserFeedback::noticed_problem) ||
                feedback > static_cast<int>(
                    IncidentUserFeedback::did_not_notice_problem)) {
                return std::unexpected{simple_error(
                    StorageErrorCode::invalid_data,
                    "invalid feedback calibration history")};
            }
            context.history.push_back(StoredFeedbackCalibrationObservation{
                sqlite3_column_int64(history->get(), 0),
                sqlite3_column_int64(history->get(), 1),
                static_cast<core::AutomaticIncidentResource>(resource),
                static_cast<core::AutomaticIncidentSignal>(signal),
                static_cast<IncidentUserFeedback>(feedback)});
        }
        return context;
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(
            StorageErrorCode::sql_error,
            "unknown feedback calibration query failure")};
    }
}

std::expected<FeedbackProfileControlState, StorageError>
SqliteIncidentArchive::reset_feedback_profile() noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto statement = prepare(database, R"sql(
UPDATE feedback_profile_state
SET revision=revision+1,
    previous_reset_after_utc_ms=reset_after_utc_ms,
    reset_after_utc_ms=?,rollback_available=1
WHERE singleton=1 AND revision<9223372036854775807
RETURNING revision,reset_after_utc_ms,rollback_available
)sql", "prepare feedback profile reset");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_int64(statement->get(), 1, now);
        const auto result = sqlite3_step(statement->get());
        if (result != SQLITE_ROW) {
            return std::unexpected{result == SQLITE_DONE
                ? simple_error(StorageErrorCode::invalid_data,
                               "feedback profile cannot advance")
                : database_error(database, "reset feedback profile", result)};
        }
        const auto revision = sqlite3_column_int64(statement->get(), 0);
        const auto reset_after = sqlite3_column_int64(statement->get(), 1);
        const auto rollback = sqlite3_column_int(statement->get(), 2);
        if (revision < 0 || reset_after != now || rollback != 1) {
            return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                 "invalid feedback profile reset state")};
        }
        const auto done = sqlite3_step(statement->get());
        if (done != SQLITE_DONE) {
            return std::unexpected{database_error(
                database, "finish feedback profile reset", done)};
        }
        return FeedbackProfileControlState{
            static_cast<std::uint64_t>(revision), reset_after, true};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown feedback profile reset failure")};
    }
}

std::expected<FeedbackProfileControlState, StorageError>
SqliteIncidentArchive::rollback_feedback_profile_reset() noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        auto statement = prepare(database, R"sql(
UPDATE feedback_profile_state
SET revision=revision+1,reset_after_utc_ms=previous_reset_after_utc_ms,
    rollback_available=0
WHERE singleton=1 AND rollback_available=1 AND revision<9223372036854775807
RETURNING revision,reset_after_utc_ms,rollback_available
)sql", "prepare feedback profile reset rollback");
        if (!statement) return std::unexpected{statement.error()};
        const auto result = sqlite3_step(statement->get());
        if (result != SQLITE_ROW) {
            return std::unexpected{result == SQLITE_DONE
                ? simple_error(StorageErrorCode::invalid_data,
                               "no feedback profile reset is available to roll back")
                : database_error(database, "roll back feedback profile reset", result)};
        }
        const auto revision = sqlite3_column_int64(statement->get(), 0);
        const auto reset_after = sqlite3_column_int64(statement->get(), 1);
        const auto rollback = sqlite3_column_int(statement->get(), 2);
        if (revision < 0 || reset_after < 0 || rollback != 0) {
            return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                 "invalid feedback profile rollback state")};
        }
        const auto done = sqlite3_step(statement->get());
        if (done != SQLITE_DONE) {
            return std::unexpected{database_error(
                database, "finish feedback profile reset rollback", done)};
        }
        return FeedbackProfileControlState{
            static_cast<std::uint64_t>(revision), reset_after, false};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(
            StorageErrorCode::sql_error,
            "unknown feedback profile reset rollback failure")};
    }
}

std::expected<ContributorFeedbackContext, StorageError>
SqliteIncidentArchive::contributor_feedback_context(
    const std::int64_t incident_id,
    const std::span<const std::string> executable_keys,
    const std::size_t maximum_observations) const noexcept {
    try {
        if (incident_id <= 0 || maximum_observations == 0U ||
            maximum_observations > maximum_contributor_feedback_observations ||
            executable_keys.size() > maximum_process_profile_query_identities) {
            return std::unexpected{simple_error(
                StorageErrorCode::invalid_data,
                "invalid contributor feedback query")};
        }
        std::set<std::string, std::less<>> unique_keys;
        for (const auto& key : executable_keys) {
            if (key.empty() || key.size() > maximum_process_profile_key_bytes) {
                return std::unexpected{simple_error(
                    StorageErrorCode::invalid_data,
                    "invalid contributor feedback executable key")};
            }
            unique_keys.insert(key);
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        auto incident = prepare(database,
                                "SELECT created_utc_ms FROM incidents WHERE id=?",
                                "prepare contributor feedback incident lookup");
        if (!incident) return std::unexpected{incident.error()};
        sqlite3_bind_int64(incident->get(), 1, incident_id);
        const auto incident_result = sqlite3_step(incident->get());
        if (incident_result != SQLITE_ROW) {
            return std::unexpected{incident_result == SQLITE_DONE
                ? simple_error(StorageErrorCode::invalid_data, "incident not found")
                : database_error(database, "load contributor feedback incident",
                                 incident_result)};
        }
        ContributorFeedbackContext context{};
        context.incident_id = incident_id;
        context.incident_utc_milliseconds =
            sqlite3_column_int64(incident->get(), 0);
        if (unique_keys.empty()) return context;

        std::string placeholders;
        for (std::size_t index = 0U; index < unique_keys.size(); ++index) {
            if (index != 0U) placeholders += ',';
            placeholders += '?';
        }
        const auto read_rows = [&](const std::string& sql,
                                   const bool history)
            -> std::expected<std::vector<StoredContributorFeedbackObservation>,
                             StorageError> {
            auto statement = prepare(database, sql.c_str(),
                                     history
                                         ? "prepare contributor feedback history"
                                         : "prepare current contributor feedback");
            if (!statement) return std::unexpected{statement.error()};
            int parameter = 1;
            if (!history) sqlite3_bind_int64(statement->get(), parameter++, incident_id);
            for (const auto& key : unique_keys) {
                sqlite3_bind_text(statement->get(), parameter++, key.c_str(),
                                  static_cast<int>(key.size()), SQLITE_TRANSIENT);
            }
            if (history) {
                sqlite3_bind_int64(statement->get(), parameter++,
                                   context.incident_utc_milliseconds);
                sqlite3_bind_int64(statement->get(), parameter++,
                                   context.incident_utc_milliseconds);
                sqlite3_bind_int64(statement->get(), parameter++, incident_id);
                sqlite3_bind_int64(statement->get(), parameter++,
                                   static_cast<sqlite3_int64>(maximum_observations));
            }
            std::vector<StoredContributorFeedbackObservation> rows;
            rows.reserve(maximum_observations);
            while (true) {
                const auto result = sqlite3_step(statement->get());
                if (result == SQLITE_DONE) break;
                if (result != SQLITE_ROW) {
                    return std::unexpected{database_error(
                        database, history ? "load contributor feedback history"
                                          : "load current contributor feedback",
                        result)};
                }
                const auto* key = sqlite3_column_text(statement->get(), 3);
                const auto resource = sqlite3_column_int(statement->get(), 4);
                const auto disposition = sqlite3_column_int(statement->get(), 5);
                const auto relationship = sqlite3_column_int(statement->get(), 6);
                if (key == nullptr || resource < 0 || resource > 3 ||
                    disposition < 1 || disposition > 2 ||
                    relationship < 0 || relationship > 2) {
                    return std::unexpected{simple_error(
                        StorageErrorCode::invalid_data,
                        "invalid contributor feedback row")};
                }
                rows.push_back(StoredContributorFeedbackObservation{
                    sqlite3_column_int64(statement->get(), 0),
                    sqlite3_column_int64(statement->get(), 1),
                    sqlite3_column_int64(statement->get(), 2),
                    reinterpret_cast<const char*>(key),
                    static_cast<ContributorFeedbackResource>(resource),
                    static_cast<ContributorFeedbackDisposition>(disposition),
                    static_cast<ContributorFeedbackTemporalRelationship>(
                        relationship)});
            }
            return rows;
        };
        const std::string columns =
            "SELECT f.incident_id,i.created_utc_ms,f.updated_utc_ms,"
            "f.executable_key,f.resource,f.disposition,f.temporal_relationship "
            "FROM incident_contributor_feedback f "
            "JOIN incidents i ON i.id=f.incident_id ";
        auto current = read_rows(
            columns + "WHERE f.incident_id=? AND f.executable_key IN (" +
                placeholders + ") ORDER BY f.executable_key,f.resource LIMIT 256",
            false);
        if (!current) return std::unexpected{current.error()};
        auto history = read_rows(
            columns + "WHERE f.executable_key IN (" + placeholders +
                ") AND (i.created_utc_ms<? OR (i.created_utc_ms=? AND i.id<?)) "
                "ORDER BY i.created_utc_ms DESC,i.id DESC,f.executable_key,f.resource "
                "LIMIT ?",
            true);
        if (!history) return std::unexpected{history.error()};
        context.current = std::move(*current);
        context.history = std::move(*history);
        return context;
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(
            StorageErrorCode::sql_error,
            "unknown contributor feedback load failure")};
    }
}

std::expected<void, StorageError>
SqliteIncidentArchive::update_contributor_feedback(
    const std::int64_t incident_id, std::string executable_key,
    const ContributorFeedbackResource resource,
    const ContributorFeedbackDisposition disposition,
    const ContributorFeedbackTemporalRelationship temporal_relationship) noexcept {
    try {
        if (incident_id <= 0 || executable_key.empty() ||
            executable_key.size() > maximum_process_profile_key_bytes ||
            static_cast<unsigned>(resource) > 3U ||
            static_cast<unsigned>(disposition) > 2U ||
            static_cast<unsigned>(temporal_relationship) > 2U) {
            return std::unexpected{simple_error(
                StorageErrorCode::invalid_data,
                "invalid contributor feedback update")};
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        auto incident = prepare(database, "SELECT 1 FROM incidents WHERE id=?",
                                "prepare contributor feedback incident validation");
        if (!incident) return std::unexpected{incident.error()};
        sqlite3_bind_int64(incident->get(), 1, incident_id);
        const auto found = sqlite3_step(incident->get());
        if (found != SQLITE_ROW) {
            return std::unexpected{found == SQLITE_DONE
                ? simple_error(StorageErrorCode::invalid_data, "incident not found")
                : database_error(database, "validate contributor feedback incident",
                                 found)};
        }
        if (disposition == ContributorFeedbackDisposition::unsure) {
            auto statement = prepare(database,
                "DELETE FROM incident_contributor_feedback "
                "WHERE incident_id=? AND executable_key=? AND resource=?",
                "prepare contributor feedback clear");
            if (!statement) return std::unexpected{statement.error()};
            sqlite3_bind_int64(statement->get(), 1, incident_id);
            sqlite3_bind_text(statement->get(), 2, executable_key.c_str(),
                              static_cast<int>(executable_key.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int(statement->get(), 3, static_cast<int>(resource));
            return expect_done(database, statement->get(),
                               "clear contributor feedback");
        }
        auto statement = prepare(database, R"sql(
INSERT INTO incident_contributor_feedback(
 incident_id,executable_key,resource,disposition,temporal_relationship,updated_utc_ms)
VALUES(?,?,?,?,?,?)
ON CONFLICT(incident_id,executable_key,resource) DO UPDATE SET
 disposition=excluded.disposition,
 temporal_relationship=excluded.temporal_relationship,
 updated_utc_ms=excluded.updated_utc_ms
)sql", "prepare contributor feedback update");
        if (!statement) return std::unexpected{statement.error()};
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        sqlite3_bind_int64(statement->get(), 1, incident_id);
        sqlite3_bind_text(statement->get(), 2, executable_key.c_str(),
                          static_cast<int>(executable_key.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(statement->get(), 3, static_cast<int>(resource));
        sqlite3_bind_int(statement->get(), 4, static_cast<int>(disposition));
        sqlite3_bind_int(statement->get(), 5,
                         static_cast<int>(temporal_relationship));
        sqlite3_bind_int64(statement->get(), 6, now);
        return expect_done(database, statement->get(),
                           "update contributor feedback");
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(
            StorageErrorCode::sql_error,
            "unknown contributor feedback update failure")};
    }
}

std::expected<std::optional<std::int64_t>, StorageError>
SqliteIncidentArchive::incident_id_for_export_key(const IncidentExportKey& key) const noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        auto statement = prepare(database, "SELECT id FROM incidents WHERE export_key=?",
                                 "prepare incident export key lookup");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_blob(statement->get(), 1, key.bytes.data(),
                          static_cast<int>(key.bytes.size()), SQLITE_TRANSIENT);
        const auto result = sqlite3_step(statement->get());
        if (result == SQLITE_DONE) return std::optional<std::int64_t>{};
        if (result != SQLITE_ROW) {
            return std::unexpected{database_error(database,
                                                  "lookup incident export key", result)};
        }
        return std::optional<std::int64_t>{sqlite3_column_int64(statement->get(), 0)};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown incident export key lookup failure")};
    }
}

std::expected<ProcessProfileContext, StorageError>
SqliteIncidentArchive::process_profile_context(
    const std::int64_t incident_id,
    const std::span<const std::string> executable_keys) const noexcept {
    try {
        if (executable_keys.size() > maximum_process_profile_query_identities) {
            return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                 "too many process profile keys")};
        }
        for (const auto& key : executable_keys) {
            if (key.empty() || key.size() > maximum_process_profile_key_bytes) {
                return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                     "invalid process profile key")};
            }
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        auto incident_time = prepare(database,
                                     "SELECT created_utc_ms FROM incidents WHERE id=?",
                                     "prepare process profile incident time");
        if (!incident_time) return std::unexpected{incident_time.error()};
        sqlite3_bind_int64(incident_time->get(), 1, incident_id);
        const auto time_result = sqlite3_step(incident_time->get());
        if (time_result != SQLITE_ROW) {
            return std::unexpected{time_result == SQLITE_DONE
                ? simple_error(StorageErrorCode::invalid_data, "incident not found")
                : database_error(database, "load process profile incident time", time_result)};
        }
        ProcessProfileContext context{};
        context.incident_id = incident_id;
        context.incident_utc_milliseconds = sqlite3_column_int64(incident_time->get(), 0);
        const auto age = process_profile_maximum_age.count();
        const auto oldest = context.incident_utc_milliseconds <
                                    std::numeric_limits<std::int64_t>::min() + age
                                ? std::numeric_limits<std::int64_t>::min()
                                : context.incident_utc_milliseconds - age;
        auto history = prepare(database, R"sql(
SELECT o.executable_key,p.display_name,o.incident_id,o.observed_utc_ms,
       o.cpu_fraction,o.working_set_bytes,o.disk_read_bps,o.disk_write_bps
FROM executable_profile_observations o
JOIN executable_profiles p ON p.executable_key=o.executable_key
WHERE o.executable_key=?
      AND (o.observed_utc_ms<? OR (o.observed_utc_ms=? AND o.incident_id<?))
      AND o.observed_utc_ms>=?
ORDER BY o.observed_utc_ms DESC,o.incident_id DESC
LIMIT 64
)sql", "prepare process profile history load");
        if (!history) return std::unexpected{history.error()};
        std::set<std::string, std::less<>> unique_keys{executable_keys.begin(),
                                                       executable_keys.end()};
        context.history.reserve(unique_keys.size() *
                                maximum_process_profile_observations_per_identity);
        const auto optional_double = [](sqlite3_stmt* statement,
                                        const int column) -> std::optional<double> {
            if (sqlite3_column_type(statement, column) == SQLITE_NULL) return std::nullopt;
            return sqlite3_column_double(statement, column);
        };
        for (const auto& key : unique_keys) {
            sqlite3_bind_text(history->get(), 1, key.c_str(),
                              static_cast<int>(key.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(history->get(), 2, context.incident_utc_milliseconds);
            sqlite3_bind_int64(history->get(), 3, context.incident_utc_milliseconds);
            sqlite3_bind_int64(history->get(), 4, incident_id);
            sqlite3_bind_int64(history->get(), 5, oldest);
            while (true) {
                const auto result = sqlite3_step(history->get());
                if (result == SQLITE_DONE) break;
                if (result != SQLITE_ROW) {
                    return std::unexpected{database_error(
                        database, "load process profile history", result)};
                }
                const auto* stored_key = sqlite3_column_text(history->get(), 0);
                const auto* display_name = sqlite3_column_text(history->get(), 1);
                if (stored_key == nullptr || display_name == nullptr) {
                    return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                         "invalid process profile text")};
                }
                context.history.push_back(StoredProcessProfileObservation{
                    reinterpret_cast<const char*>(stored_key),
                    reinterpret_cast<const char*>(display_name),
                    sqlite3_column_int64(history->get(), 2),
                    sqlite3_column_int64(history->get(), 3),
                    optional_double(history->get(), 4),
                    optional_double(history->get(), 5),
                    optional_double(history->get(), 6),
                    optional_double(history->get(), 7)});
            }
            sqlite3_reset(history->get());
            sqlite3_clear_bindings(history->get());
        }
        std::sort(context.history.begin(), context.history.end(),
                  [](const auto& left, const auto& right) {
                      if (left.executable_key != right.executable_key) {
                          return left.executable_key < right.executable_key;
                      }
                      if (left.observed_utc_milliseconds != right.observed_utc_milliseconds) {
                          return left.observed_utc_milliseconds < right.observed_utc_milliseconds;
                      }
                      return left.incident_id < right.incident_id;
                  });
        return context;
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown process profile load failure")};
    }
}

std::expected<void, StorageError> SqliteIncidentArchive::store_process_profile_updates(
    const std::int64_t incident_id,
    const std::span<const ProcessProfileUpdate> updates) noexcept {
    try {
        if (updates.size() > maximum_process_profile_query_identities) {
            return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                 "too many process profile updates")};
        }
        std::set<std::string, std::less<>> unique_keys;
        const auto valid_metric = [](const std::optional<double> value,
                                     const double maximum) noexcept {
            return !value || (std::isfinite(*value) && *value >= 0.0 && *value <= maximum);
        };
        for (const auto& update : updates) {
            if (update.executable_key.empty() ||
                update.executable_key.size() > maximum_process_profile_key_bytes ||
                update.display_name.size() > maximum_process_profile_display_name_bytes ||
                !unique_keys.insert(update.executable_key).second ||
                !valid_metric(update.cpu_fraction, 1.0) ||
                !valid_metric(update.working_set_bytes,
                              static_cast<double>(std::numeric_limits<std::uint64_t>::max())) ||
                !valid_metric(update.disk_read_bytes_per_second,
                              std::numeric_limits<double>::max()) ||
                !valid_metric(update.disk_write_bytes_per_second,
                              std::numeric_limits<double>::max())) {
                return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                     "invalid process profile update")};
            }
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        Transaction transaction{database};
        if (auto begun = transaction.begin(); !begun) return begun;
        auto incident_time_statement = prepare(
            database, "SELECT created_utc_ms FROM incidents WHERE id=?",
            "prepare profile update incident time");
        if (!incident_time_statement) {
            return std::unexpected{incident_time_statement.error()};
        }
        sqlite3_bind_int64(incident_time_statement->get(), 1, incident_id);
        const auto time_result = sqlite3_step(incident_time_statement->get());
        if (time_result != SQLITE_ROW) {
            return std::unexpected{time_result == SQLITE_DONE
                ? simple_error(StorageErrorCode::invalid_data, "incident not found")
                : database_error(database, "load profile update incident time", time_result)};
        }
        const auto incident_time = sqlite3_column_int64(incident_time_statement->get(), 0);
        const auto age = process_profile_maximum_age.count();
        const auto oldest = incident_time < std::numeric_limits<std::int64_t>::min() + age
                                ? std::numeric_limits<std::int64_t>::min()
                                : incident_time - age;
        auto prune_old = prepare(database,
                                 "DELETE FROM executable_profile_observations "
                                 "WHERE observed_utc_ms<?",
                                 "prepare old process profile pruning");
        if (!prune_old) return std::unexpected{prune_old.error()};
        sqlite3_bind_int64(prune_old->get(), 1, oldest);
        if (auto pruned = expect_done(database, prune_old->get(),
                                      "prune old process profile observations"); !pruned) {
            return pruned;
        }
        if (auto pruned = execute(database,
                "DELETE FROM executable_profiles WHERE NOT EXISTS ("
                "SELECT 1 FROM executable_profile_observations o "
                "WHERE o.executable_key=executable_profiles.executable_key)",
                "prune empty process profiles"); !pruned) {
            return pruned;
        }

        auto exists = prepare(database,
                              "SELECT 1 FROM executable_profiles WHERE executable_key=?",
                              "prepare process profile existence check");
        auto upsert_profile = prepare(database, R"sql(
INSERT INTO executable_profiles(executable_key,display_name,last_seen_utc_ms)
VALUES(?,?,?)
ON CONFLICT(executable_key) DO UPDATE SET
 display_name=excluded.display_name,
 last_seen_utc_ms=MAX(executable_profiles.last_seen_utc_ms,excluded.last_seen_utc_ms)
)sql", "prepare process profile upsert");
        auto insert_observation = prepare(database, R"sql(
INSERT INTO executable_profile_observations(
 executable_key,incident_id,observed_utc_ms,cpu_fraction,working_set_bytes,
 disk_read_bps,disk_write_bps)
VALUES(?,?,?,?,?,?,?) ON CONFLICT(executable_key,incident_id) DO NOTHING
)sql", "prepare process profile observation insert");
        auto trim_observations = prepare(database, R"sql(
DELETE FROM executable_profile_observations
WHERE executable_key=? AND incident_id NOT IN (
 SELECT incident_id FROM executable_profile_observations
 WHERE executable_key=?
 ORDER BY observed_utc_ms DESC,incident_id DESC LIMIT 64)
)sql", "prepare process profile observation trim");
        if (!exists) return std::unexpected{exists.error()};
        if (!upsert_profile) return std::unexpected{upsert_profile.error()};
        if (!insert_observation) return std::unexpected{insert_observation.error()};
        if (!trim_observations) return std::unexpected{trim_observations.error()};

        std::vector<const ProcessProfileUpdate*> ordered;
        ordered.reserve(updates.size());
        for (const auto& update : updates) ordered.push_back(&update);
        std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
            return left->executable_key < right->executable_key;
        });
        const auto bind_optional = [](sqlite3_stmt* statement, const int index,
                                      const std::optional<double> value) noexcept {
            return value ? sqlite3_bind_double(statement, index, *value)
                         : sqlite3_bind_null(statement, index);
        };
        for (const auto* update : ordered) {
            sqlite3_bind_text(exists->get(), 1, update->executable_key.c_str(),
                              static_cast<int>(update->executable_key.size()), SQLITE_TRANSIENT);
            const auto exists_result = sqlite3_step(exists->get());
            if (exists_result != SQLITE_ROW && exists_result != SQLITE_DONE) {
                return std::unexpected{database_error(database,
                    "check process profile existence", exists_result)};
            }
            const auto new_identity = exists_result == SQLITE_DONE;
            sqlite3_reset(exists->get());
            sqlite3_clear_bindings(exists->get());
            if (new_identity) {
                auto count = scalar_int64(database,
                                          "SELECT COUNT(*) FROM executable_profiles",
                                          "count process profiles");
                if (!count) return std::unexpected{count.error()};
                if (static_cast<std::uint64_t>(*count) >=
                    maximum_process_profile_identities) {
                    if (auto evicted = execute(database,
                            "DELETE FROM executable_profiles WHERE executable_key=("
                            "SELECT executable_key FROM executable_profiles "
                            "ORDER BY last_seen_utc_ms,executable_key LIMIT 1)",
                            "evict oldest process profile"); !evicted) {
                        return evicted;
                    }
                }
            }
            sqlite3_bind_text(upsert_profile->get(), 1, update->executable_key.c_str(),
                              static_cast<int>(update->executable_key.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(upsert_profile->get(), 2, update->display_name.c_str(),
                              static_cast<int>(update->display_name.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(upsert_profile->get(), 3, incident_time);
            if (auto stored = expect_done(database, upsert_profile->get(),
                                          "upsert process profile"); !stored) return stored;

            sqlite3_bind_text(insert_observation->get(), 1,
                              update->executable_key.c_str(),
                              static_cast<int>(update->executable_key.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(insert_observation->get(), 2, incident_id);
            sqlite3_bind_int64(insert_observation->get(), 3, incident_time);
            bind_optional(insert_observation->get(), 4, update->cpu_fraction);
            bind_optional(insert_observation->get(), 5, update->working_set_bytes);
            bind_optional(insert_observation->get(), 6,
                          update->disk_read_bytes_per_second);
            bind_optional(insert_observation->get(), 7,
                          update->disk_write_bytes_per_second);
            if (auto stored = expect_done(database, insert_observation->get(),
                                          "store process profile observation"); !stored) {
                return stored;
            }
            sqlite3_bind_text(trim_observations->get(), 1,
                              update->executable_key.c_str(),
                              static_cast<int>(update->executable_key.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(trim_observations->get(), 2,
                              update->executable_key.c_str(),
                              static_cast<int>(update->executable_key.size()), SQLITE_TRANSIENT);
            if (auto trimmed = expect_done(database, trim_observations->get(),
                                           "trim process profile observations"); !trimmed) {
                return trimmed;
            }
        }
        return transaction.commit();
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown process profile update failure")};
    }
}

std::expected<std::vector<StoredRecurringIncident>, StorageError>
SqliteIncidentArchive::recurring_incidents(const std::size_t maximum_results) const noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        const auto limit = std::clamp<std::size_t>(maximum_results, 1U,
                                                   maximum_recurring_incidents);
        auto statement = prepare(database, R"sql(
SELECT recent.id,recent.created_utc_ms,recent.label,recent.recurring_group_override,
       recent.user_feedback,recent.category,
       feature.feature_version,feature.feature_index,feature.value,feature.available
FROM (
 SELECT id,created_utc_ms,label,recurring_group_override,user_feedback,category
 FROM incidents
 ORDER BY created_utc_ms DESC,id DESC LIMIT ?
) AS recent
LEFT JOIN incident_feature_cache AS feature ON feature.incident_id=recent.id
ORDER BY recent.created_utc_ms DESC,recent.id DESC,feature.feature_index
)sql", "prepare recurring incident load");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_int64(statement->get(), 1, static_cast<sqlite3_int64>(limit));

        std::vector<StoredRecurringIncident> incidents;
        incidents.reserve(limit);
        while (true) {
            const auto result = sqlite3_step(statement->get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                return std::unexpected{database_error(database,
                    "load recurring incidents", result)};
            }
            const auto incident_id = sqlite3_column_int64(statement->get(), 0);
            if (incidents.empty() || incidents.back().id != incident_id) {
                StoredRecurringIncident incident{};
                incident.id = incident_id;
                incident.created_utc_milliseconds =
                    sqlite3_column_int64(statement->get(), 1);
                const auto* label = sqlite3_column_text(statement->get(), 2);
                const auto* override_group = sqlite3_column_text(statement->get(), 3);
                incident.label = label == nullptr
                    ? "" : reinterpret_cast<const char*>(label);
                incident.override_group = override_group == nullptr
                    ? "" : reinterpret_cast<const char*>(override_group);
                const auto feedback = sqlite3_column_int(statement->get(), 4);
                const auto category = sqlite3_column_int(statement->get(), 5);
                if (feedback < 0 ||
                    feedback > static_cast<int>(
                        IncidentUserFeedback::did_not_notice_problem) ||
                    category < 0 ||
                    category > static_cast<int>(IncidentCategory::audio)) {
                    return std::unexpected{simple_error(
                        StorageErrorCode::invalid_data,
                        "invalid recurring incident classification")};
                }
                incident.user_feedback =
                    static_cast<IncidentUserFeedback>(feedback);
                incident.category = static_cast<IncidentCategory>(category);
                incidents.push_back(std::move(incident));
            }
            if (sqlite3_column_type(statement->get(), 6) == SQLITE_NULL) continue;
            const auto version = sqlite3_column_int(statement->get(), 6);
            const auto index = sqlite3_column_int(statement->get(), 7);
            if (version <= 0 || index < 0 ||
                index >= static_cast<int>(maximum_incident_feature_dimensions)) {
                return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                     "invalid cached incident feature")};
            }
            auto& cached = incidents.back().cached_feature;
            if (!cached) cached = StoredIncidentFeatureCache{incident_id, version, {}, {}};
            if (cached->feature_version != version ||
                static_cast<std::size_t>(index) != cached->values.size()) {
                return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                    "inconsistent cached incident feature")};
            }
            const auto value = sqlite3_column_double(statement->get(), 8);
            const auto available = sqlite3_column_int(statement->get(), 9);
            if (!std::isfinite(value) || (available != 0 && available != 1)) {
                return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                     "invalid incident feature value")};
            }
            cached->values.push_back(value);
            cached->available.push_back(static_cast<std::uint8_t>(available));
        }
        return incidents;
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown recurring incident load failure")};
    }
}

std::expected<std::string, StorageError>
SqliteIncidentArchive::recurring_group_override(
    const std::int64_t incident_id) const noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        auto statement = prepare(database,
            "SELECT recurring_group_override FROM incidents WHERE id=?",
            "prepare recurring override load");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_int64(statement->get(), 1, incident_id);
        const auto result = sqlite3_step(statement->get());
        if (result != SQLITE_ROW) {
            return std::unexpected{result == SQLITE_DONE
                ? simple_error(StorageErrorCode::invalid_data, "incident not found")
                : database_error(database, "load recurring override", result)};
        }
        const auto* text = sqlite3_column_text(statement->get(), 0);
        return text == nullptr ? std::string{} :
                                 std::string{reinterpret_cast<const char*>(text)};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown recurring override load failure")};
    }
}

std::expected<void, StorageError> SqliteIncidentArchive::store_incident_features(
    const std::span<const StoredIncidentFeatureCache> features) noexcept {
    try {
        if (features.size() > maximum_recurring_incidents) {
            return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                 "too many incident features")};
        }
        std::set<std::int64_t> identities;
        for (const auto& feature : features) {
            if (feature.incident_id <= 0 || feature.feature_version <= 0 ||
                feature.values.empty() ||
                feature.values.size() > maximum_incident_feature_dimensions ||
                feature.values.size() != feature.available.size() ||
                !identities.insert(feature.incident_id).second) {
                return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                     "invalid incident feature")};
            }
            for (std::size_t index = 0U; index < feature.values.size(); ++index) {
                if (!std::isfinite(feature.values[index]) || feature.available[index] > 1U) {
                    return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                         "invalid incident feature value")};
                }
            }
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        if (features.empty()) return {};
        Transaction transaction{database};
        if (auto begun = transaction.begin(); !begun) return begun;
        auto remove = prepare(database,
            "DELETE FROM incident_feature_cache WHERE incident_id=?",
            "prepare incident feature replacement");
        auto insert = prepare(database, R"sql(
INSERT INTO incident_feature_cache(
 incident_id,feature_version,feature_index,value,available) VALUES(?,?,?,?,?)
)sql", "prepare incident feature insert");
        if (!remove) return std::unexpected{remove.error()};
        if (!insert) return std::unexpected{insert.error()};
        for (const auto& feature : features) {
            sqlite3_bind_int64(remove->get(), 1, feature.incident_id);
            if (auto removed = expect_done(database, remove->get(),
                                           "replace incident feature"); !removed) {
                return removed;
            }
            for (std::size_t index = 0U; index < feature.values.size(); ++index) {
                sqlite3_bind_int64(insert->get(), 1, feature.incident_id);
                sqlite3_bind_int(insert->get(), 2, feature.feature_version);
                sqlite3_bind_int(insert->get(), 3, static_cast<int>(index));
                sqlite3_bind_double(insert->get(), 4, feature.values[index]);
                sqlite3_bind_int(insert->get(), 5, feature.available[index]);
                if (auto stored = expect_done(database, insert->get(),
                                              "store incident feature"); !stored) {
                    return stored;
                }
            }
        }
        if (auto pruned = execute(database,
                "DELETE FROM incident_feature_cache WHERE incident_id NOT IN ("
                "SELECT id FROM incidents ORDER BY created_utc_ms DESC,id DESC LIMIT 512)",
                "prune incident feature cache"); !pruned) {
            return pruned;
        }
        return transaction.commit();
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown incident feature store failure")};
    }
}

std::expected<void, StorageError>
SqliteIncidentArchive::update_recurring_group_override(
    const std::int64_t incident_id, std::string override_group) noexcept {
    try {
        if (incident_id <= 0 ||
            override_group.size() > maximum_recurring_group_override_bytes) {
            return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                 "invalid recurring group override")};
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        auto statement = prepare(database,
            "UPDATE incidents SET recurring_group_override=? WHERE id=?",
            "prepare recurring override update");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_text(statement->get(), 1, override_group.c_str(),
                          static_cast<int>(override_group.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement->get(), 2, incident_id);
        if (auto updated = expect_done(database, statement->get(),
                                       "update recurring override"); !updated) {
            return updated;
        }
        if (sqlite3_changes(database) == 0) {
            return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                 "incident not found")};
        }
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown recurring override update failure")};
    }
}

std::expected<ProcessProfileStorageStatistics, StorageError>
SqliteIncidentArchive::process_profile_storage_statistics() const noexcept {
    const std::scoped_lock lock{native_->mutex};
    if (native_->database == nullptr) {
        return std::unexpected{simple_error(StorageErrorCode::not_open,
                                             "incident archive is not open")};
    }
    auto identities = scalar_int64(native_->database,
                                   "SELECT COUNT(*) FROM executable_profiles",
                                   "count process profiles");
    if (!identities) return std::unexpected{identities.error()};
    auto observations = scalar_int64(
        native_->database, "SELECT COUNT(*) FROM executable_profile_observations",
        "count process profile observations");
    if (!observations) return std::unexpected{observations.error()};
    return ProcessProfileStorageStatistics{
        static_cast<std::uint64_t>(*identities),
        static_cast<std::uint64_t>(*observations)};
}

std::expected<std::uint64_t, StorageError> SqliteIncidentArchive::incident_count() const noexcept {
    const std::scoped_lock lock{native_->mutex};
    if (native_->database == nullptr) {
        return std::unexpected{simple_error(StorageErrorCode::not_open,
                                             "incident archive is not open")};
    }
    auto count = scalar_int64(native_->database, "SELECT COUNT(*) FROM incidents",
                              "count incidents");
    if (!count) return std::unexpected{count.error()};
    return static_cast<std::uint64_t>(*count);
}

std::expected<std::int32_t, StorageError> SqliteIncidentArchive::schema_version() const noexcept {
    const std::scoped_lock lock{native_->mutex};
    if (native_->database == nullptr) {
        return std::unexpected{simple_error(StorageErrorCode::not_open,
                                             "incident archive is not open")};
    }
    auto version = scalar_int64(native_->database, "PRAGMA user_version",
                                "read schema version");
    if (!version) return std::unexpected{version.error()};
    return static_cast<std::int32_t>(*version);
}

std::expected<std::uint64_t, StorageError>
SqliteIncidentArchive::database_size_bytes() const noexcept {
    const std::scoped_lock lock{native_->mutex};
    if (native_->database == nullptr) {
        return std::unexpected{simple_error(StorageErrorCode::not_open,
                                             "incident archive is not open")};
    }
    auto pages = scalar_int64(native_->database, "PRAGMA page_count", "read page count");
    auto size = scalar_int64(native_->database, "PRAGMA page_size", "read page size");
    if (!pages) return std::unexpected{pages.error()};
    if (!size) return std::unexpected{size.error()};
    return static_cast<std::uint64_t>(*pages) * static_cast<std::uint64_t>(*size);
}

std::expected<ArchiveMaintenanceResult, StorageError>
SqliteIncidentArchive::apply_retention(const ArchiveRetentionPolicy& policy) noexcept {
    try {
        if ((!policy.delete_before_utc_milliseconds && !policy.maximum_incidents) ||
            (policy.maximum_incidents && *policy.maximum_incidents == 0U)) {
            return std::unexpected{simple_error(
                StorageErrorCode::invalid_data,
                "retention requires an age or a positive incident limit")};
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        const auto before = scalar_int64(database, "SELECT COUNT(*) FROM incidents",
                                         "count incidents before retention");
        if (!before) return std::unexpected{before.error()};

        Transaction transaction{database};
        if (auto begun = transaction.begin(); !begun) return std::unexpected{begun.error()};
        if (policy.delete_before_utc_milliseconds) {
            auto statement = prepare(
                database, "DELETE FROM incidents WHERE created_utc_ms < ?",
                "prepare age-based incident retention");
            if (!statement) return std::unexpected{statement.error()};
            sqlite3_bind_int64(statement->get(), 1, *policy.delete_before_utc_milliseconds);
            if (auto deleted = expect_done(database, statement->get(),
                                           "apply age-based incident retention"); !deleted) {
                return std::unexpected{deleted.error()};
            }
        }
        if (policy.maximum_incidents) {
            auto statement = prepare(database, R"sql(
DELETE FROM incidents WHERE id NOT IN (
 SELECT id FROM incidents ORDER BY created_utc_ms DESC,id DESC LIMIT ?
)
)sql", "prepare count-based incident retention");
            if (!statement) return std::unexpected{statement.error()};
            sqlite3_bind_int64(statement->get(), 1,
                               static_cast<sqlite3_int64>(*policy.maximum_incidents));
            if (auto deleted = expect_done(database, statement->get(),
                                           "apply count-based incident retention"); !deleted) {
                return std::unexpected{deleted.error()};
            }
        }
        if (auto cleaned = execute(
                database,
                "DELETE FROM executable_profiles WHERE executable_key NOT IN ("
                "SELECT DISTINCT executable_key FROM executable_profile_observations)",
                "remove orphaned process profiles"); !cleaned) {
            return std::unexpected{cleaned.error()};
        }
        if (auto committed = transaction.commit(); !committed) {
            return std::unexpected{committed.error()};
        }
        const auto remaining = scalar_int64(database, "SELECT COUNT(*) FROM incidents",
                                            "count incidents after retention");
        if (!remaining) return std::unexpected{remaining.error()};
        if (auto checkpoint = execute(database, "PRAGMA wal_checkpoint(TRUNCATE)",
                                      "truncate archive WAL after retention"); !checkpoint) {
            return std::unexpected{checkpoint.error()};
        }
        if (policy.compact_after_delete && *remaining < *before) {
            if (auto compacted = execute(database, "VACUUM", "compact incident archive");
                !compacted) {
                return std::unexpected{compacted.error()};
            }
        }
        auto pages = scalar_int64(database, "PRAGMA page_count", "read page count");
        auto page_size = scalar_int64(database, "PRAGMA page_size", "read page size");
        if (!pages) return std::unexpected{pages.error()};
        if (!page_size) return std::unexpected{page_size.error()};
        return ArchiveMaintenanceResult{
            static_cast<std::uint64_t>(*before - *remaining),
            static_cast<std::uint64_t>(*remaining),
            static_cast<std::uint64_t>(*pages) * static_cast<std::uint64_t>(*page_size)};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown archive retention failure")};
    }
}

std::expected<ArchiveMaintenanceResult, StorageError>
SqliteIncidentArchive::purge_all_incidents() noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{simple_error(StorageErrorCode::not_open,
                                                 "incident archive is not open")};
        }
        const auto before = scalar_int64(database, "SELECT COUNT(*) FROM incidents",
                                         "count incidents before privacy purge");
        if (!before) return std::unexpected{before.error()};
        Transaction transaction{database};
        if (auto begun = transaction.begin(); !begun) return std::unexpected{begun.error()};
        if (auto deleted = execute(database, "DELETE FROM incidents",
                                   "purge all incidents"); !deleted) {
            return std::unexpected{deleted.error()};
        }
        if (auto profiles = execute(database, "DELETE FROM executable_profiles",
                                    "purge process profiles"); !profiles) {
            return std::unexpected{profiles.error()};
        }
        if (auto feedback = execute(database, R"sql(
UPDATE feedback_profile_state
SET revision=0,reset_after_utc_ms=0,previous_reset_after_utc_ms=0,
    rollback_available=0
WHERE singleton=1
)sql", "purge feedback profile control state"); !feedback) {
            return std::unexpected{feedback.error()};
        }
        if (auto committed = transaction.commit(); !committed) {
            return std::unexpected{committed.error()};
        }
        if (auto checkpoint = execute(database, "PRAGMA wal_checkpoint(TRUNCATE)",
                                      "truncate archive WAL after privacy purge"); !checkpoint) {
            return std::unexpected{checkpoint.error()};
        }
        if (auto compacted = execute(database, "VACUUM", "compact purged incident archive");
            !compacted) {
            return std::unexpected{compacted.error()};
        }
        auto pages = scalar_int64(database, "PRAGMA page_count", "read page count");
        auto page_size = scalar_int64(database, "PRAGMA page_size", "read page size");
        if (!pages) return std::unexpected{pages.error()};
        if (!page_size) return std::unexpected{page_size.error()};
        return ArchiveMaintenanceResult{
            static_cast<std::uint64_t>(*before), 0U,
            static_cast<std::uint64_t>(*pages) * static_cast<std::uint64_t>(*page_size)};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                             "unknown archive privacy purge failure")};
    }
}

const ArchiveConfiguration& SqliteIncidentArchive::configuration() const noexcept {
    return configuration_;
}

} // namespace blackbox::storage
