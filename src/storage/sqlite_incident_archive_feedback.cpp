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

std::expected<IncidentAnnotation, StorageError>
SqliteIncidentArchive::annotation(const std::int64_t incident_id) const noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        auto statement = prepare(database,
                                 "SELECT label,note,user_feedback,category "
                                 "FROM incidents WHERE id=?",
                                 "prepare incident annotation load");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_int64(statement->get(), 1, incident_id);
        const auto result = sqlite3_step(statement->get());
        if (result != SQLITE_ROW) {
            return std::unexpected{
                result == SQLITE_DONE
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
            return std::unexpected{
                simple_error(StorageErrorCode::invalid_data, "invalid incident classification")};
        }
        return IncidentAnnotation{label == nullptr ? "" : reinterpret_cast<const char*>(label),
                                  note == nullptr ? "" : reinterpret_cast<const char*>(note),
                                  static_cast<IncidentUserFeedback>(feedback),
                                  static_cast<IncidentCategory>(category)};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{
            simple_error(StorageErrorCode::sql_error, "unknown annotation load failure")};
    }
}

std::expected<void, StorageError>
SqliteIncidentArchive::update_annotation(const std::int64_t incident_id,
                                         const IncidentAnnotation& annotation_value) noexcept {
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
            return std::unexpected{
                simple_error(StorageErrorCode::invalid_data, "invalid incident annotation")};
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        Transaction transaction{database};
        if (auto begun = transaction.begin(); !begun) {
            return std::unexpected{begun.error()};
        }
        auto previous = prepare(database, "SELECT category,user_feedback FROM incidents WHERE id=?",
                                "prepare previous incident classification");
        if (!previous) return std::unexpected{previous.error()};
        sqlite3_bind_int64(previous->get(), 1, incident_id);
        const auto previous_result = sqlite3_step(previous->get());
        if (previous_result != SQLITE_ROW) {
            return std::unexpected{
                previous_result == SQLITE_DONE
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
                                 "UPDATE incidents SET "
                                 "label=?,note=?,user_feedback=?,category=? WHERE id=?",
                                 "prepare incident annotation update");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_text(statement->get(), 1, annotation_value.label.c_str(),
                          static_cast<int>(annotation_value.label.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(statement->get(), 2, annotation_value.note.c_str(),
                          static_cast<int>(annotation_value.note.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(statement->get(), 3, static_cast<int>(annotation_value.user_feedback));
        sqlite3_bind_int(statement->get(), 4, static_cast<int>(annotation_value.category));
        sqlite3_bind_int64(statement->get(), 5, incident_id);
        if (auto updated = expect_done(database, statement->get(), "update incident annotation");
            !updated) {
            return std::unexpected{updated.error()};
        }
        if (sqlite3_changes(database) != 1) {
            return std::unexpected{
                simple_error(StorageErrorCode::invalid_data, "incident not found")};
        }
        if (classification_changed) {
            auto history_insert = prepare(database, R"sql(
INSERT INTO incident_classification_history(
 incident_id, changed_utc_ms, category, user_feedback, origin)
VALUES(?,?,?,?,?)
)sql",
                                          "prepare incident classification history insert");
            if (!history_insert) return std::unexpected{history_insert.error()};
            const auto changed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
            sqlite3_bind_int64(history_insert->get(), 1, incident_id);
            sqlite3_bind_int64(history_insert->get(), 2, changed);
            sqlite3_bind_int(history_insert->get(), 3, static_cast<int>(annotation_value.category));
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
)sql",
                                 "prepare incident classification history prune");
            if (!prune) return std::unexpected{prune.error()};
            sqlite3_bind_int64(prune->get(), 1, incident_id);
            sqlite3_bind_int64(prune->get(), 2, incident_id);
            sqlite3_bind_int64(prune->get(), 3,
                               static_cast<sqlite3_int64>(maximum_incident_classification_history));
            if (auto pruned =
                    expect_done(database, prune->get(), "prune incident classification history");
                !pruned) {
                return std::unexpected{pruned.error()};
            }
        }
        return transaction.commit();
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{
            simple_error(StorageErrorCode::sql_error, "unknown annotation update failure")};
    }
}

std::expected<std::vector<IncidentClassificationHistoryEntry>, StorageError>
SqliteIncidentArchive::classification_history(const std::int64_t incident_id) const noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        auto statement = prepare(database, R"sql(
SELECT changed_utc_ms,category,user_feedback,origin
FROM incident_classification_history WHERE incident_id=?
ORDER BY changed_utc_ms,event_id
)sql",
                                 "prepare incident classification history load");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_int64(statement->get(), 1, incident_id);
        std::vector<IncidentClassificationHistoryEntry> history;
        history.reserve(maximum_incident_classification_history);
        while (true) {
            const auto result = sqlite3_step(statement->get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                return std::unexpected{
                    database_error(database, "load incident classification history", result)};
            }
            const auto category = sqlite3_column_int(statement->get(), 1);
            const auto feedback = sqlite3_column_int(statement->get(), 2);
            const auto origin = sqlite3_column_int(statement->get(), 3);
            if (category < 0 || category > static_cast<int>(IncidentCategory::audio) ||
                feedback < 0 ||
                feedback > static_cast<int>(IncidentUserFeedback::did_not_notice_problem) ||
                origin < 0 ||
                origin > static_cast<int>(ClassificationChangeOrigin::dataset_import)) {
                return std::unexpected{
                    simple_error(StorageErrorCode::invalid_data, "invalid classification history")};
            }
            history.push_back(IncidentClassificationHistoryEntry{
                sqlite3_column_int64(statement->get(), 0), static_cast<IncidentCategory>(category),
                static_cast<IncidentUserFeedback>(feedback),
                static_cast<ClassificationChangeOrigin>(origin)});
        }
        return history;
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{
            simple_error(StorageErrorCode::sql_error, "unknown classification history failure")};
    }
}

std::expected<FeedbackCalibrationContext, StorageError>
SqliteIncidentArchive::feedback_calibration_context(
    const std::int64_t incident_id, const std::size_t maximum_observations) const noexcept {
    try {
        if (incident_id <= 0 || maximum_observations == 0U ||
            maximum_observations > maximum_feedback_calibration_observations) {
            return std::unexpected{
                simple_error(StorageErrorCode::invalid_data, "invalid feedback calibration query")};
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        auto current = prepare(database, "SELECT created_utc_ms FROM incidents WHERE id=?",
                               "prepare feedback calibration incident load");
        if (!current) return std::unexpected{current.error()};
        sqlite3_bind_int64(current->get(), 1, incident_id);
        const auto current_result = sqlite3_step(current->get());
        if (current_result != SQLITE_ROW) {
            return std::unexpected{
                current_result == SQLITE_DONE
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
)sql",
                               "prepare feedback profile state load");
        if (!profile) return std::unexpected{profile.error()};
        const auto profile_result = sqlite3_step(profile->get());
        if (profile_result != SQLITE_ROW || sqlite3_column_int64(profile->get(), 0) < 0 ||
            sqlite3_column_int64(profile->get(), 1) < 0 ||
            (sqlite3_column_int(profile->get(), 2) != 0 &&
             sqlite3_column_int(profile->get(), 2) != 1)) {
            return std::unexpected{
                profile_result == SQLITE_ROW
                    ? simple_error(StorageErrorCode::invalid_data, "invalid feedback profile state")
                    : database_error(database, "load feedback profile state", profile_result)};
        }
        context.profile_revision =
            static_cast<std::uint64_t>(sqlite3_column_int64(profile->get(), 0));
        context.reset_after_utc_milliseconds = sqlite3_column_int64(profile->get(), 1);
        context.rollback_available = sqlite3_column_int(profile->get(), 2) != 0;

        auto history = prepare(database, R"sql(
SELECT id,created_utc_ms,automatic_resource,automatic_signal,user_feedback
FROM incidents
WHERE automatic_trigger_count>0 AND user_feedback<>0
  AND created_utc_ms>?
  AND (created_utc_ms<? OR (created_utc_ms=? AND id<?))
ORDER BY created_utc_ms DESC,id DESC LIMIT ?
)sql",
                               "prepare feedback calibration history load");
        if (!history) return std::unexpected{history.error()};
        sqlite3_bind_int64(history->get(), 1, context.reset_after_utc_milliseconds);
        sqlite3_bind_int64(history->get(), 2, context.incident_utc_milliseconds);
        sqlite3_bind_int64(history->get(), 3, context.incident_utc_milliseconds);
        sqlite3_bind_int64(history->get(), 4, incident_id);
        sqlite3_bind_int64(history->get(), 5, static_cast<sqlite3_int64>(maximum_observations));
        context.history.reserve(maximum_observations);
        while (true) {
            const auto result = sqlite3_step(history->get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                return std::unexpected{
                    database_error(database, "load feedback calibration history", result)};
            }
            const auto resource = sqlite3_column_int(history->get(), 2);
            const auto signal = sqlite3_column_int(history->get(), 3);
            const auto feedback = sqlite3_column_int(history->get(), 4);
            if (resource < static_cast<int>(core::AutomaticIncidentResource::none) ||
                resource > static_cast<int>(core::AutomaticIncidentResource::network) ||
                signal <
                    static_cast<int>(core::AutomaticIncidentSignal::throughput_or_utilization) ||
                signal > static_cast<int>(core::AutomaticIncidentSignal::storage_io_retry) ||
                feedback < static_cast<int>(IncidentUserFeedback::noticed_problem) ||
                feedback > static_cast<int>(IncidentUserFeedback::did_not_notice_problem)) {
                return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                    "invalid feedback calibration history")};
            }
            context.history.push_back(StoredFeedbackCalibrationObservation{
                sqlite3_column_int64(history->get(), 0), sqlite3_column_int64(history->get(), 1),
                static_cast<core::AutomaticIncidentResource>(resource),
                static_cast<core::AutomaticIncidentSignal>(signal),
                static_cast<IncidentUserFeedback>(feedback)});
        }
        return context;
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                            "unknown feedback calibration query failure")};
    }
}

std::expected<FeedbackProfileControlState, StorageError>
SqliteIncidentArchive::reset_feedback_profile() noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        auto statement = prepare(database, R"sql(
UPDATE feedback_profile_state
SET revision=revision+1,
    previous_reset_after_utc_ms=reset_after_utc_ms,
    reset_after_utc_ms=?,rollback_available=1
WHERE singleton=1 AND revision<9223372036854775807
RETURNING revision,reset_after_utc_ms,rollback_available
)sql",
                                 "prepare feedback profile reset");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_int64(statement->get(), 1, now);
        const auto result = sqlite3_step(statement->get());
        if (result != SQLITE_ROW) {
            return std::unexpected{
                result == SQLITE_DONE ? simple_error(StorageErrorCode::invalid_data,
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
            return std::unexpected{database_error(database, "finish feedback profile reset", done)};
        }
        return FeedbackProfileControlState{static_cast<std::uint64_t>(revision), reset_after, true};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{
            simple_error(StorageErrorCode::sql_error, "unknown feedback profile reset failure")};
    }
}

std::expected<FeedbackProfileControlState, StorageError>
SqliteIncidentArchive::rollback_feedback_profile_reset() noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        auto statement = prepare(database, R"sql(
UPDATE feedback_profile_state
SET revision=revision+1,reset_after_utc_ms=previous_reset_after_utc_ms,
    rollback_available=0
WHERE singleton=1 AND rollback_available=1 AND revision<9223372036854775807
RETURNING revision,reset_after_utc_ms,rollback_available
)sql",
                                 "prepare feedback profile reset rollback");
        if (!statement) return std::unexpected{statement.error()};
        const auto result = sqlite3_step(statement->get());
        if (result != SQLITE_ROW) {
            return std::unexpected{
                result == SQLITE_DONE
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
            return std::unexpected{
                database_error(database, "finish feedback profile reset rollback", done)};
        }
        return FeedbackProfileControlState{static_cast<std::uint64_t>(revision), reset_after,
                                           false};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                            "unknown feedback profile reset rollback failure")};
    }
}

std::expected<ContributorFeedbackContext, StorageError>
SqliteIncidentArchive::contributor_feedback_context(
    const std::int64_t incident_id, const std::span<const std::string> executable_keys,
    const std::size_t maximum_observations) const noexcept {
    try {
        if (incident_id <= 0 || maximum_observations == 0U ||
            maximum_observations > maximum_contributor_feedback_observations ||
            executable_keys.size() > maximum_process_profile_query_identities) {
            return std::unexpected{
                simple_error(StorageErrorCode::invalid_data, "invalid contributor feedback query")};
        }
        std::set<std::string, std::less<>> unique_keys;
        for (const auto& key : executable_keys) {
            if (key.empty() || key.size() > maximum_process_profile_key_bytes) {
                return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                    "invalid contributor feedback executable key")};
            }
            unique_keys.insert(key);
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        auto incident = prepare(database, "SELECT created_utc_ms FROM incidents WHERE id=?",
                                "prepare contributor feedback incident lookup");
        if (!incident) return std::unexpected{incident.error()};
        sqlite3_bind_int64(incident->get(), 1, incident_id);
        const auto incident_result = sqlite3_step(incident->get());
        if (incident_result != SQLITE_ROW) {
            return std::unexpected{
                incident_result == SQLITE_DONE
                    ? simple_error(StorageErrorCode::invalid_data, "incident not found")
                    : database_error(database, "load contributor feedback incident",
                                     incident_result)};
        }
        ContributorFeedbackContext context{};
        context.incident_id = incident_id;
        context.incident_utc_milliseconds = sqlite3_column_int64(incident->get(), 0);
        if (unique_keys.empty()) return context;

        std::string placeholders;
        for (std::size_t index = 0U; index < unique_keys.size(); ++index) {
            if (index != 0U) placeholders += ',';
            placeholders += '?';
        }
        const auto read_rows = [&](const std::string& sql, const bool history)
            -> std::expected<std::vector<StoredContributorFeedbackObservation>, StorageError> {
            auto statement = prepare(database, sql.c_str(),
                                     history ? "prepare contributor feedback history"
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
                    return std::unexpected{database_error(database,
                                                          history
                                                              ? "load contributor feedback history"
                                                              : "load current contributor feedback",
                                                          result)};
                }
                const auto* key = sqlite3_column_text(statement->get(), 3);
                const auto resource = sqlite3_column_int(statement->get(), 4);
                const auto disposition = sqlite3_column_int(statement->get(), 5);
                const auto relationship = sqlite3_column_int(statement->get(), 6);
                if (key == nullptr || resource < 0 || resource > 3 || disposition < 1 ||
                    disposition > 2 || relationship < 0 || relationship > 2) {
                    return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                        "invalid contributor feedback row")};
                }
                rows.push_back(StoredContributorFeedbackObservation{
                    sqlite3_column_int64(statement->get(), 0),
                    sqlite3_column_int64(statement->get(), 1),
                    sqlite3_column_int64(statement->get(), 2), reinterpret_cast<const char*>(key),
                    static_cast<ContributorFeedbackResource>(resource),
                    static_cast<ContributorFeedbackDisposition>(disposition),
                    static_cast<ContributorFeedbackTemporalRelationship>(relationship)});
            }
            return rows;
        };
        const std::string columns =
            "SELECT f.incident_id,i.created_utc_ms,f.updated_utc_ms,"
            "f.executable_key,f.resource,f.disposition,f.temporal_relationship "
            "FROM incident_contributor_feedback f "
            "JOIN incidents i ON i.id=f.incident_id ";
        auto current =
            read_rows(columns + "WHERE f.incident_id=? AND f.executable_key IN (" + placeholders +
                          ") ORDER BY f.executable_key,f.resource LIMIT 256",
                      false);
        if (!current) return std::unexpected{current.error()};
        auto history =
            read_rows(columns + "WHERE f.executable_key IN (" + placeholders +
                          ") AND (i.created_utc_ms<? OR (i.created_utc_ms=? AND i.id<?)) "
                          "ORDER BY i.created_utc_ms DESC,i.id "
                          "DESC,f.executable_key,f.resource "
                          "LIMIT ?",
                      true);
        if (!history) return std::unexpected{history.error()};
        context.current = std::move(*current);
        context.history = std::move(*history);
        return context;
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{
            simple_error(StorageErrorCode::sql_error, "unknown contributor feedback load failure")};
    }
}

std::expected<void, StorageError> SqliteIncidentArchive::update_contributor_feedback(
    const std::int64_t incident_id, std::string executable_key,
    const ContributorFeedbackResource resource, const ContributorFeedbackDisposition disposition,
    const ContributorFeedbackTemporalRelationship temporal_relationship) noexcept {
    try {
        if (incident_id <= 0 || executable_key.empty() ||
            executable_key.size() > maximum_process_profile_key_bytes ||
            static_cast<unsigned>(resource) > 3U || static_cast<unsigned>(disposition) > 2U ||
            static_cast<unsigned>(temporal_relationship) > 2U) {
            return std::unexpected{simple_error(StorageErrorCode::invalid_data,
                                                "invalid contributor feedback update")};
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        auto incident = prepare(database, "SELECT 1 FROM incidents WHERE id=?",
                                "prepare contributor feedback incident validation");
        if (!incident) return std::unexpected{incident.error()};
        sqlite3_bind_int64(incident->get(), 1, incident_id);
        const auto found = sqlite3_step(incident->get());
        if (found != SQLITE_ROW) {
            return std::unexpected{
                found == SQLITE_DONE
                    ? simple_error(StorageErrorCode::invalid_data, "incident not found")
                    : database_error(database, "validate contributor feedback incident", found)};
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
            return expect_done(database, statement->get(), "clear contributor feedback");
        }
        auto statement = prepare(database, R"sql(
INSERT INTO incident_contributor_feedback(
 incident_id,executable_key,resource,disposition,temporal_relationship,updated_utc_ms)
VALUES(?,?,?,?,?,?)
ON CONFLICT(incident_id,executable_key,resource) DO UPDATE SET
 disposition=excluded.disposition,
 temporal_relationship=excluded.temporal_relationship,
 updated_utc_ms=excluded.updated_utc_ms
)sql",
                                 "prepare contributor feedback update");
        if (!statement) return std::unexpected{statement.error()};
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        sqlite3_bind_int64(statement->get(), 1, incident_id);
        sqlite3_bind_text(statement->get(), 2, executable_key.c_str(),
                          static_cast<int>(executable_key.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(statement->get(), 3, static_cast<int>(resource));
        sqlite3_bind_int(statement->get(), 4, static_cast<int>(disposition));
        sqlite3_bind_int(statement->get(), 5, static_cast<int>(temporal_relationship));
        sqlite3_bind_int64(statement->get(), 6, now);
        return expect_done(database, statement->get(), "update contributor feedback");
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                            "unknown contributor feedback update failure")};
    }
}

std::expected<std::optional<std::int64_t>, StorageError>
SqliteIncidentArchive::incident_id_for_export_key(const IncidentExportKey& key) const noexcept {
    try {
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        auto statement = prepare(database, "SELECT id FROM incidents WHERE export_key=?",
                                 "prepare incident export key lookup");
        if (!statement) return std::unexpected{statement.error()};
        sqlite3_bind_blob(statement->get(), 1, key.bytes.data(), static_cast<int>(key.bytes.size()),
                          SQLITE_TRANSIENT);
        const auto result = sqlite3_step(statement->get());
        if (result == SQLITE_DONE) return std::optional<std::int64_t>{};
        if (result != SQLITE_ROW) {
            return std::unexpected{database_error(database, "lookup incident export key", result)};
        }
        return std::optional<std::int64_t>{sqlite3_column_int64(statement->get(), 0)};
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{simple_error(StorageErrorCode::sql_error,
                                            "unknown incident export key lookup failure")};
    }
}

std::expected<ProcessProfileContext, StorageError> SqliteIncidentArchive::process_profile_context(
    const std::int64_t incident_id,
    const std::span<const std::string> executable_keys) const noexcept {
    try {
        if (executable_keys.size() > maximum_process_profile_query_identities) {
            return std::unexpected{
                simple_error(StorageErrorCode::invalid_data, "too many process profile keys")};
        }
        for (const auto& key : executable_keys) {
            if (key.empty() || key.size() > maximum_process_profile_key_bytes) {
                return std::unexpected{
                    simple_error(StorageErrorCode::invalid_data, "invalid process profile key")};
            }
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        auto incident_time = prepare(database, "SELECT created_utc_ms FROM incidents WHERE id=?",
                                     "prepare process profile incident time");
        if (!incident_time) return std::unexpected{incident_time.error()};
        sqlite3_bind_int64(incident_time->get(), 1, incident_id);
        const auto time_result = sqlite3_step(incident_time->get());
        if (time_result != SQLITE_ROW) {
            return std::unexpected{
                time_result == SQLITE_DONE
                    ? simple_error(StorageErrorCode::invalid_data, "incident not found")
                    : database_error(database, "load process profile incident time", time_result)};
        }
        ProcessProfileContext context{};
        context.incident_id = incident_id;
        context.incident_utc_milliseconds = sqlite3_column_int64(incident_time->get(), 0);
        const auto age = process_profile_maximum_age.count();
        const auto oldest =
            context.incident_utc_milliseconds < std::numeric_limits<std::int64_t>::min() + age
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
)sql",
                               "prepare process profile history load");
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
            sqlite3_bind_text(history->get(), 1, key.c_str(), static_cast<int>(key.size()),
                              SQLITE_TRANSIENT);
            sqlite3_bind_int64(history->get(), 2, context.incident_utc_milliseconds);
            sqlite3_bind_int64(history->get(), 3, context.incident_utc_milliseconds);
            sqlite3_bind_int64(history->get(), 4, incident_id);
            sqlite3_bind_int64(history->get(), 5, oldest);
            while (true) {
                const auto result = sqlite3_step(history->get());
                if (result == SQLITE_DONE) break;
                if (result != SQLITE_ROW) {
                    return std::unexpected{
                        database_error(database, "load process profile history", result)};
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
                    sqlite3_column_int64(history->get(), 3), optional_double(history->get(), 4),
                    optional_double(history->get(), 5), optional_double(history->get(), 6),
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
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{
            simple_error(StorageErrorCode::sql_error, "unknown process profile load failure")};
    }
}

std::expected<void, StorageError> SqliteIncidentArchive::store_process_profile_updates(
    const std::int64_t incident_id, const std::span<const ProcessProfileUpdate> updates) noexcept {
    try {
        if (updates.size() > maximum_process_profile_query_identities) {
            return std::unexpected{
                simple_error(StorageErrorCode::invalid_data, "too many process profile updates")};
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
                return std::unexpected{
                    simple_error(StorageErrorCode::invalid_data, "invalid process profile update")};
            }
        }
        const std::scoped_lock lock{native_->mutex};
        auto* database = native_->database;
        if (database == nullptr) {
            return std::unexpected{
                simple_error(StorageErrorCode::not_open, "incident archive is not open")};
        }
        Transaction transaction{database};
        if (auto begun = transaction.begin(); !begun) return begun;
        auto incident_time_statement =
            prepare(database, "SELECT created_utc_ms FROM incidents WHERE id=?",
                    "prepare profile update incident time");
        if (!incident_time_statement) {
            return std::unexpected{incident_time_statement.error()};
        }
        sqlite3_bind_int64(incident_time_statement->get(), 1, incident_id);
        const auto time_result = sqlite3_step(incident_time_statement->get());
        if (time_result != SQLITE_ROW) {
            return std::unexpected{
                time_result == SQLITE_DONE
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
        if (auto pruned =
                expect_done(database, prune_old->get(), "prune old process profile observations");
            !pruned) {
            return pruned;
        }
        if (auto pruned = execute(database,
                                  "DELETE FROM executable_profiles WHERE NOT EXISTS ("
                                  "SELECT 1 FROM executable_profile_observations o "
                                  "WHERE o.executable_key=executable_profiles.executable_key)",
                                  "prune empty process profiles");
            !pruned) {
            return pruned;
        }

        auto exists = prepare(database, "SELECT 1 FROM executable_profiles WHERE executable_key=?",
                              "prepare process profile existence check");
        auto upsert_profile = prepare(database, R"sql(
INSERT INTO executable_profiles(executable_key,display_name,last_seen_utc_ms)
VALUES(?,?,?)
ON CONFLICT(executable_key) DO UPDATE SET
 display_name=excluded.display_name,
 last_seen_utc_ms=MAX(executable_profiles.last_seen_utc_ms,excluded.last_seen_utc_ms)
)sql",
                                      "prepare process profile upsert");
        auto insert_observation = prepare(database, R"sql(
INSERT INTO executable_profile_observations(
 executable_key,incident_id,observed_utc_ms,cpu_fraction,working_set_bytes,
 disk_read_bps,disk_write_bps)
VALUES(?,?,?,?,?,?,?) ON CONFLICT(executable_key,incident_id) DO NOTHING
)sql",
                                          "prepare process profile observation insert");
        auto trim_observations = prepare(database, R"sql(
DELETE FROM executable_profile_observations
WHERE executable_key=? AND incident_id NOT IN (
 SELECT incident_id FROM executable_profile_observations
 WHERE executable_key=?
 ORDER BY observed_utc_ms DESC,incident_id DESC LIMIT 64)
)sql",
                                         "prepare process profile observation trim");
        if (!exists) return std::unexpected{exists.error()};
        if (!upsert_profile) return std::unexpected{upsert_profile.error()};
        if (!insert_observation) return std::unexpected{insert_observation.error()};
        if (!trim_observations) return std::unexpected{trim_observations.error()};

        std::vector<const ProcessProfileUpdate*> ordered;
        ordered.reserve(updates.size());
        for (const auto& update : updates)
            ordered.push_back(&update);
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
                return std::unexpected{
                    database_error(database, "check process profile existence", exists_result)};
            }
            const auto new_identity = exists_result == SQLITE_DONE;
            sqlite3_reset(exists->get());
            sqlite3_clear_bindings(exists->get());
            if (new_identity) {
                auto count = scalar_int64(database, "SELECT COUNT(*) FROM executable_profiles",
                                          "count process profiles");
                if (!count) return std::unexpected{count.error()};
                if (static_cast<std::uint64_t>(*count) >= maximum_process_profile_identities) {
                    if (auto evicted = execute(database,
                                               "DELETE FROM executable_profiles WHERE "
                                               "executable_key=("
                                               "SELECT executable_key FROM executable_profiles "
                                               "ORDER BY last_seen_utc_ms,executable_key LIMIT 1)",
                                               "evict oldest process profile");
                        !evicted) {
                        return evicted;
                    }
                }
            }
            sqlite3_bind_text(upsert_profile->get(), 1, update->executable_key.c_str(),
                              static_cast<int>(update->executable_key.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(upsert_profile->get(), 2, update->display_name.c_str(),
                              static_cast<int>(update->display_name.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(upsert_profile->get(), 3, incident_time);
            if (auto stored =
                    expect_done(database, upsert_profile->get(), "upsert process profile");
                !stored)
                return stored;

            sqlite3_bind_text(insert_observation->get(), 1, update->executable_key.c_str(),
                              static_cast<int>(update->executable_key.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(insert_observation->get(), 2, incident_id);
            sqlite3_bind_int64(insert_observation->get(), 3, incident_time);
            bind_optional(insert_observation->get(), 4, update->cpu_fraction);
            bind_optional(insert_observation->get(), 5, update->working_set_bytes);
            bind_optional(insert_observation->get(), 6, update->disk_read_bytes_per_second);
            bind_optional(insert_observation->get(), 7, update->disk_write_bytes_per_second);
            if (auto stored = expect_done(database, insert_observation->get(),
                                          "store process profile observation");
                !stored) {
                return stored;
            }
            sqlite3_bind_text(trim_observations->get(), 1, update->executable_key.c_str(),
                              static_cast<int>(update->executable_key.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(trim_observations->get(), 2, update->executable_key.c_str(),
                              static_cast<int>(update->executable_key.size()), SQLITE_TRANSIENT);
            if (auto trimmed = expect_done(database, trim_observations->get(),
                                           "trim process profile observations");
                !trimmed) {
                return trimmed;
            }
        }
        return transaction.commit();
    } catch (const std::exception& exception) {
        return std::unexpected{StorageError{StorageErrorCode::sql_error, 0, exception.what()}};
    } catch (...) {
        return std::unexpected{
            simple_error(StorageErrorCode::sql_error, "unknown process profile update failure")};
    }
}

} // namespace blackbox::storage
