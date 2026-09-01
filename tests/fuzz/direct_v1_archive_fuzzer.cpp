#include "storage/sqlite_incident_archive_internal.hpp"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    constexpr std::size_t maximum_input_size = 1024U * 1024U;
    if (data == nullptr || size == 0U || size > maximum_input_size) return 0;

    sqlite3* database{};
    if (sqlite3_open_v2(":memory:", &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK) {
        if (database != nullptr) sqlite3_close(database);
        return 0;
    }

    auto* copy = static_cast<unsigned char*>(sqlite3_malloc64(size));
    if (copy == nullptr) {
        sqlite3_close(database);
        return 0;
    }
    std::memcpy(copy, data, size);
    const auto loaded = sqlite3_deserialize(
        database, "main", copy, static_cast<sqlite3_int64>(size),
        static_cast<sqlite3_int64>(size), SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_READONLY);
    if (loaded == SQLITE_OK) {
        static_cast<void>(blackbox::storage::detail::validate_direct_schema_v1(database));
    } else {
        sqlite3_free(copy);
    }
    sqlite3_close(database);
    return 0;
}
