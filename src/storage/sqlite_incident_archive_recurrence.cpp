#include "storage/incident_archive.hpp"
#include "storage/sqlite_incident_archive_internal.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace blackbox::storage {

using namespace detail;

std::expected<std::vector<StoredRecurringIncident>, StorageError>
SqliteIncidentArchive::recurring_incidents(const std::size_t maximum_results) const noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        const auto limit =
            std::clamp<std::size_t>(maximum_results, 1U, maximum_recurring_incidents);
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
)sql",
                                 "prepare recurring incident load");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_int64(statement->get(), 1, static_cast<sqlite3_int64>(limit));

        std::vector<StoredRecurringIncident> incidents;
        incidents.reserve(limit);
        while (true) {
            const auto result = sqlite3_step(statement->get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                return std::unexpected{
                    database_error(database, "load recurring incidents", result)};
            }
            const auto incident_id = sqlite3_column_int64(statement->get(), 0);
            if (incidents.empty() || incidents.back().id != incident_id) {
                StoredRecurringIncident incident{};
                incident.id = incident_id;
                incident.created_utc_milliseconds = sqlite3_column_int64(statement->get(), 1);
                const auto* label = sqlite3_column_text(statement->get(), 2);
                const auto* override_group = sqlite3_column_text(statement->get(), 3);
                incident.label = label == nullptr ? "" : reinterpret_cast<const char*>(label);
                incident.override_group =
                    override_group == nullptr ? "" : reinterpret_cast<const char*>(override_group);
                const auto feedback = sqlite3_column_int(statement->get(), 4);
                const auto category = sqlite3_column_int(statement->get(), 5);
                if (feedback < 0 ||
                    feedback > static_cast<int>(IncidentUserFeedback::did_not_notice_problem) ||
                    category < 0 || category > static_cast<int>(IncidentCategory::audio)) {
                    return std::unexpected{
                        simple_error(StorageErrorCode::invalid_data,
                                     "invalid recurring incident classification")};
                }
                incident.user_feedback = static_cast<IncidentUserFeedback>(feedback);
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
                return std::unexpected{
                    simple_error(StorageErrorCode::invalid_data, "invalid incident feature value")};
            }
            cached->values.push_back(value);
            cached->available.push_back(static_cast<std::uint8_t>(available));
        }
        return incidents;
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{
            simple_error(StorageErrorCode::sql_error, "unknown recurring incident load failure")};
    }
}

std::expected<std::string, StorageError>
SqliteIncidentArchive::recurring_group_override(const std::int64_t incident_id) const noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        auto statement =
            prepare(database, "SELECT recurring_group_override FROM incidents WHERE id=?",
                    "prepare recurring override load");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_int64(statement->get(), 1, incident_id);
        const auto result = sqlite3_step(statement->get());
        if (result != SQLITE_ROW) {
            return std::unexpected{
                result == SQLITE_DONE
                    ? simple_error(StorageErrorCode::invalid_data, "incident not found")
                    : database_error(database, "load recurring override", result)};
        }
        const auto* text = sqlite3_column_text(statement->get(), 0);
        return text == nullptr ? std::string{} : std::string{reinterpret_cast<const char*>(text)};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{
            simple_error(StorageErrorCode::sql_error, "unknown recurring override load failure")};
    }
}

std::expected<void, StorageError> SqliteIncidentArchive::store_incident_features(
    const std::span<const StoredIncidentFeatureCache> features) noexcept {
    try {
        if (features.size() > maximum_recurring_incidents) {
            return std::unexpected{
                simple_error(StorageErrorCode::invalid_data, "too many incident features")};
        }
        std::set<std::int64_t> identities;
        for (const auto& feature : features) {
            if (feature.incident_id <= 0 || feature.feature_version <= 0 ||
                feature.values.empty() ||
                feature.values.size() > maximum_incident_feature_dimensions ||
                feature.values.size() != feature.available.size() ||
                !identities.insert(feature.incident_id).second) {
                return std::unexpected{
                    simple_error(StorageErrorCode::invalid_data, "invalid incident feature")};
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
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        if (features.empty()) return {};
        Transaction transaction{database};
        if (auto begun = transaction.begin(); !begun) return begun;
        auto remove = prepare(database, "DELETE FROM incident_feature_cache WHERE incident_id=?",
                              "prepare incident feature replacement");
        auto insert = prepare(database, R"sql(
INSERT INTO incident_feature_cache(
 incident_id,feature_version,feature_index,value,available) VALUES(?,?,?,?,?)
)sql",
                              "prepare incident feature insert");
        if (!remove) return std::unexpected{remove.error()};
        if (!insert) return std::unexpected{insert.error()};
        for (const auto& feature : features) {
            sqlite3_bind_int64(remove->get(), 1, feature.incident_id);
            if (auto removed = expect_done(database, remove->get(), "replace incident feature");
                !removed) {
                return removed;
            }
            for (std::size_t index = 0U; index < feature.values.size(); ++index) {
                sqlite3_bind_int64(insert->get(), 1, feature.incident_id);
                sqlite3_bind_int(insert->get(), 2, feature.feature_version);
                sqlite3_bind_int(insert->get(), 3, static_cast<int>(index));
                sqlite3_bind_double(insert->get(), 4, feature.values[index]);
                sqlite3_bind_int(insert->get(), 5, feature.available[index]);
                if (auto stored = expect_done(database, insert->get(), "store incident feature");
                    !stored) {
                    return stored;
                }
            }
        }
        if (auto pruned = execute(database,
                                  "DELETE FROM incident_feature_cache WHERE incident_id NOT IN ("
                                  "SELECT id FROM incidents ORDER BY created_utc_ms DESC,id DESC "
                                  "LIMIT 512)",
                                  "prune incident feature cache");
            !pruned) {
            return pruned;
        }
        return transaction.commit();
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{
            simple_error(StorageErrorCode::sql_error, "unknown incident feature store failure")};
    }
}

std::expected<void, StorageError>
SqliteIncidentArchive::update_recurring_group_override(const std::int64_t incident_id,
                                                       std::string override_group) noexcept {
    try {
        if (incident_id <= 0 || override_group.size() > maximum_recurring_group_override_bytes) {
            return std::unexpected{
                simple_error(StorageErrorCode::invalid_data, "invalid recurring group override")};
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        auto statement =
            prepare(database, "UPDATE incidents SET recurring_group_override=? WHERE id=?",
                    "prepare recurring override update");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_text(statement->get(), 1, override_group.c_str(),
                          static_cast<int>(override_group.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement->get(), 2, incident_id);
        if (auto updated = expect_done(database, statement->get(), "update recurring override");
            !updated) {
            return updated;
        }
        if (sqlite3_changes(database) == 0) {
            return std::unexpected{
                simple_error(StorageErrorCode::invalid_data, "incident not found")};
        }
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{
            simple_error(StorageErrorCode::sql_error, "unknown recurring override update failure")};
    }
}

} // namespace blackbox::storage
