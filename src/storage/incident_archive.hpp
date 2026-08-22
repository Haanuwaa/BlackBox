#pragma once

#include "core/incident.hpp"

#include <chrono>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace blackbox::storage {

inline constexpr std::int32_t current_schema_version = 1;
inline constexpr std::uint64_t default_maximum_archive_bytes = 1ULL << 30U;
inline constexpr std::size_t maximum_incident_page_size = 100U;
inline constexpr std::size_t maximum_incident_label_bytes = 128U;
inline constexpr std::size_t maximum_incident_note_bytes = 4'096U;
inline constexpr std::size_t maximum_incident_classification_history = 64U;
inline constexpr std::size_t maximum_process_profile_identities = 2'048U;
inline constexpr std::size_t maximum_process_profile_observations_per_identity = 64U;
inline constexpr std::size_t maximum_process_profile_query_identities = 512U;
inline constexpr std::size_t maximum_process_profile_key_bytes = 2'048U;
inline constexpr std::size_t maximum_process_profile_display_name_bytes = 512U;
inline constexpr std::chrono::milliseconds process_profile_maximum_age{
    std::chrono::hours{24 * 30}};
inline constexpr std::size_t maximum_recurring_incidents = 512U;
inline constexpr std::size_t maximum_incident_feature_dimensions = 32U;
inline constexpr std::size_t maximum_recurring_group_override_bytes = 64U;
inline constexpr std::size_t maximum_feedback_calibration_observations = 256U;
inline constexpr std::size_t maximum_contributor_feedback_observations = 256U;

enum class StorageErrorCode : std::uint8_t {
    not_open,
    cannot_open,
    busy,
    full,
    corrupt,
    io,
    invalid_schema,
    schema_too_new,
    invalid_data,
    sql_error,
};

struct StorageError {
    StorageErrorCode code{StorageErrorCode::sql_error};
    int native_code{};
    std::string message{};
    friend bool operator==(const StorageError&, const StorageError&) = default;
};

enum class ArchiveOpenMode : std::uint8_t {
    read_write,
    read_only,
};

struct ArchiveConfiguration {
    std::filesystem::path path{};
    std::uint64_t maximum_bytes{default_maximum_archive_bytes};
    std::chrono::milliseconds busy_timeout{250};
    ArchiveOpenMode open_mode{ArchiveOpenMode::read_write};
    friend bool operator==(const ArchiveConfiguration&,
                           const ArchiveConfiguration&) = default;
};

struct IncidentExportKey {
    std::array<std::uint8_t, 16U> bytes{};
    friend constexpr auto operator<=>(const IncidentExportKey&,
                                      const IncidentExportKey&) = default;
};

struct StoredIncidentSummary {
    std::int64_t id{};
    std::int64_t created_utc_milliseconds{};
    std::uint64_t capture_sequence{};
    std::int64_t event_monotonic_nanoseconds{};
    std::int64_t actual_start_nanoseconds{};
    std::int64_t actual_end_nanoseconds{};
    std::size_t system_sample_count{};
    std::size_t process_metadata_count{};
    std::size_t process_sample_count{};
    std::string label{};
    std::string note{};
    IncidentExportKey export_key{};
    friend bool operator==(const StoredIncidentSummary&,
                           const StoredIncidentSummary&) = default;
};

enum class IncidentListSort : std::uint8_t {
    newest_first,
    oldest_first,
    longest_first,
    shortest_first,
    label_ascending,
    label_descending,
};

struct IncidentListQuery {
    std::size_t offset{};
    std::size_t limit{50U};
    std::string search{};
    IncidentListSort sort{IncidentListSort::newest_first};
    friend bool operator==(const IncidentListQuery&, const IncidentListQuery&) = default;
};

struct StoredIncidentPage {
    std::vector<StoredIncidentSummary> incidents{};
    std::uint64_t total_matching{};
    std::size_t offset{};
    friend bool operator==(const StoredIncidentPage&, const StoredIncidentPage&) = default;
};

enum class IncidentUserFeedback : std::uint8_t {
    unanswered,
    noticed_problem,
    did_not_notice_problem,
};

enum class IncidentCategory : std::uint8_t {
    unknown,
    system_freeze,
    game_stutter,
    application_slowdown_or_hang,
    network,
    audio,
};

enum class ClassificationChangeOrigin : std::uint8_t {
    capture,
    user,
    dataset_import,
};

struct IncidentAnnotation {
    std::string label{};
    std::string note{};
    IncidentUserFeedback user_feedback{IncidentUserFeedback::unanswered};
    IncidentCategory category{IncidentCategory::unknown};
    friend bool operator==(const IncidentAnnotation&, const IncidentAnnotation&) = default;
};

struct IncidentClassificationHistoryEntry {
    std::int64_t changed_utc_milliseconds{};
    IncidentCategory category{IncidentCategory::unknown};
    IncidentUserFeedback user_feedback{IncidentUserFeedback::unanswered};
    ClassificationChangeOrigin origin{ClassificationChangeOrigin::user};
    friend bool operator==(const IncidentClassificationHistoryEntry&,
                           const IncidentClassificationHistoryEntry&) = default;
};

struct StoredProcessProfileObservation {
    std::string executable_key{};
    std::string display_name{};
    std::int64_t incident_id{};
    std::int64_t observed_utc_milliseconds{};
    std::optional<double> cpu_fraction{};
    std::optional<double> working_set_bytes{};
    std::optional<double> disk_read_bytes_per_second{};
    std::optional<double> disk_write_bytes_per_second{};
    friend bool operator==(const StoredProcessProfileObservation&,
                           const StoredProcessProfileObservation&) = default;
};

struct ProcessProfileUpdate {
    std::string executable_key{};
    std::string display_name{};
    std::optional<double> cpu_fraction{};
    std::optional<double> working_set_bytes{};
    std::optional<double> disk_read_bytes_per_second{};
    std::optional<double> disk_write_bytes_per_second{};
    friend bool operator==(const ProcessProfileUpdate&,
                           const ProcessProfileUpdate&) = default;
};

struct ProcessProfileContext {
    std::int64_t incident_id{};
    std::int64_t incident_utc_milliseconds{};
    std::vector<StoredProcessProfileObservation> history{};
    friend bool operator==(const ProcessProfileContext&,
                           const ProcessProfileContext&) = default;
};

struct ProcessProfileStorageStatistics {
    std::uint64_t identity_count{};
    std::uint64_t observation_count{};
    friend bool operator==(const ProcessProfileStorageStatistics&,
                           const ProcessProfileStorageStatistics&) = default;
};

struct StoredFeedbackCalibrationObservation {
    std::int64_t incident_id{};
    std::int64_t observed_utc_milliseconds{};
    core::AutomaticIncidentResource automatic_resource{
        core::AutomaticIncidentResource::none};
    core::AutomaticIncidentSignal automatic_signal{
        core::AutomaticIncidentSignal::throughput_or_utilization};
    IncidentUserFeedback feedback{IncidentUserFeedback::unanswered};
    friend bool operator==(const StoredFeedbackCalibrationObservation&,
                           const StoredFeedbackCalibrationObservation&) = default;
};

struct FeedbackCalibrationContext {
    std::int64_t incident_id{};
    std::int64_t incident_utc_milliseconds{};
    std::uint64_t profile_revision{};
    std::int64_t reset_after_utc_milliseconds{};
    bool rollback_available{};
    std::vector<StoredFeedbackCalibrationObservation> history{};
    friend bool operator==(const FeedbackCalibrationContext&,
                           const FeedbackCalibrationContext&) = default;
};

struct FeedbackProfileControlState {
    std::uint64_t revision{};
    std::int64_t reset_after_utc_milliseconds{};
    bool rollback_available{};
    friend bool operator==(const FeedbackProfileControlState&,
                           const FeedbackProfileControlState&) = default;
};

enum class ContributorFeedbackResource : std::uint8_t {
    cpu,
    memory,
    disk,
    network,
};

enum class ContributorFeedbackDisposition : std::uint8_t {
    unsure,
    confirmed_contributor,
    not_a_contributor,
};

enum class ContributorFeedbackTemporalRelationship : std::uint8_t {
    preceding_activity,
    marker_spanning_ambiguous,
    post_marker_reaction,
};

struct StoredContributorFeedbackObservation {
    std::int64_t incident_id{};
    std::int64_t incident_utc_milliseconds{};
    std::int64_t feedback_updated_utc_milliseconds{};
    std::string executable_key{};
    ContributorFeedbackResource resource{ContributorFeedbackResource::cpu};
    ContributorFeedbackDisposition disposition{
        ContributorFeedbackDisposition::unsure};
    ContributorFeedbackTemporalRelationship temporal_relationship{
        ContributorFeedbackTemporalRelationship::preceding_activity};
    friend bool operator==(const StoredContributorFeedbackObservation&,
                           const StoredContributorFeedbackObservation&) = default;
};

struct ContributorFeedbackContext {
    std::int64_t incident_id{};
    std::int64_t incident_utc_milliseconds{};
    std::vector<StoredContributorFeedbackObservation> current{};
    std::vector<StoredContributorFeedbackObservation> history{};
    friend bool operator==(const ContributorFeedbackContext&,
                           const ContributorFeedbackContext&) = default;
};

struct ArchiveRetentionPolicy {
    std::optional<std::int64_t> delete_before_utc_milliseconds{};
    std::optional<std::size_t> maximum_incidents{};
    bool compact_after_delete{};
    friend constexpr bool operator==(const ArchiveRetentionPolicy&,
                                     const ArchiveRetentionPolicy&) = default;
};

struct ArchiveMaintenanceResult {
    std::uint64_t incidents_deleted{};
    std::uint64_t incidents_remaining{};
    std::uint64_t database_size_bytes{};
    friend constexpr bool operator==(const ArchiveMaintenanceResult&,
                                     const ArchiveMaintenanceResult&) = default;
};

struct StoredIncidentFeatureCache {
    std::int64_t incident_id{};
    std::int32_t feature_version{};
    std::vector<double> values{};
    std::vector<std::uint8_t> available{};
    friend bool operator==(const StoredIncidentFeatureCache&,
                           const StoredIncidentFeatureCache&) = default;
};

struct StoredRecurringIncident {
    std::int64_t id{};
    std::int64_t created_utc_milliseconds{};
    std::string label{};
    std::string override_group{};
    std::optional<StoredIncidentFeatureCache> cached_feature{};
    IncidentUserFeedback user_feedback{IncidentUserFeedback::unanswered};
    IncidentCategory category{IncidentCategory::unknown};
    friend bool operator==(const StoredRecurringIncident&,
                           const StoredRecurringIncident&) = default;
};

[[nodiscard]] std::filesystem::path default_archive_path();

class IIncidentArchive {
public:
    virtual ~IIncidentArchive() = default;

    [[nodiscard]] virtual std::expected<std::int64_t, StorageError> store(
        const core::IncidentSnapshot& incident) noexcept = 0;
};

class IIncidentRepository : public IIncidentArchive {
public:
    [[nodiscard]] virtual std::expected<StoredIncidentPage, StorageError> list_page(
        const IncidentListQuery& query) const noexcept = 0;
    [[nodiscard]] virtual std::expected<std::shared_ptr<const core::IncidentSnapshot>,
                                        StorageError>
    load(std::int64_t incident_id) const noexcept = 0;
    [[nodiscard]] virtual std::expected<IncidentAnnotation, StorageError> annotation(
        std::int64_t incident_id) const noexcept = 0;
    [[nodiscard]] virtual std::expected<void, StorageError> update_annotation(
        std::int64_t incident_id, const IncidentAnnotation& annotation) noexcept = 0;
};

class IProcessProfileRepository {
public:
    virtual ~IProcessProfileRepository() = default;

    [[nodiscard]] virtual std::expected<ProcessProfileContext, StorageError>
    process_profile_context(std::int64_t incident_id,
                            std::span<const std::string> executable_keys) const noexcept = 0;
    [[nodiscard]] virtual std::expected<void, StorageError>
    store_process_profile_updates(std::int64_t incident_id,
                                  std::span<const ProcessProfileUpdate> updates) noexcept = 0;
};

class IRecurringIncidentRepository {
public:
    virtual ~IRecurringIncidentRepository() = default;

    [[nodiscard]] virtual std::expected<std::vector<StoredRecurringIncident>, StorageError>
    recurring_incidents(std::size_t maximum_results = maximum_recurring_incidents)
        const noexcept = 0;
    [[nodiscard]] virtual std::expected<std::string, StorageError>
    recurring_group_override(std::int64_t incident_id) const noexcept = 0;
    [[nodiscard]] virtual std::expected<void, StorageError> store_incident_features(
        std::span<const StoredIncidentFeatureCache> features) noexcept = 0;
    [[nodiscard]] virtual std::expected<void, StorageError>
    update_recurring_group_override(std::int64_t incident_id,
                                    std::string override_group) noexcept = 0;
};

class IFeedbackCalibrationRepository {
public:
    virtual ~IFeedbackCalibrationRepository() = default;

    [[nodiscard]] virtual std::expected<FeedbackCalibrationContext, StorageError>
    feedback_calibration_context(
        std::int64_t incident_id,
        std::size_t maximum_observations =
            maximum_feedback_calibration_observations) const noexcept = 0;
    [[nodiscard]] virtual std::expected<FeedbackProfileControlState, StorageError>
    reset_feedback_profile() noexcept = 0;
    [[nodiscard]] virtual std::expected<FeedbackProfileControlState, StorageError>
    rollback_feedback_profile_reset() noexcept = 0;
    [[nodiscard]] virtual std::expected<ContributorFeedbackContext, StorageError>
    contributor_feedback_context(
        std::int64_t incident_id,
        std::span<const std::string> executable_keys,
        std::size_t maximum_observations =
            maximum_contributor_feedback_observations) const noexcept = 0;
    [[nodiscard]] virtual std::expected<void, StorageError>
    update_contributor_feedback(
        std::int64_t incident_id, std::string executable_key,
        ContributorFeedbackResource resource,
        ContributorFeedbackDisposition disposition,
        ContributorFeedbackTemporalRelationship temporal_relationship) noexcept = 0;
};

class SqliteIncidentArchive final : public IIncidentRepository,
                                    public IProcessProfileRepository,
                                    public IRecurringIncidentRepository,
                                    public IFeedbackCalibrationRepository {
public:
    explicit SqliteIncidentArchive(ArchiveConfiguration configuration);
    ~SqliteIncidentArchive() override;

    SqliteIncidentArchive(const SqliteIncidentArchive&) = delete;
    SqliteIncidentArchive& operator=(const SqliteIncidentArchive&) = delete;

    [[nodiscard]] std::expected<void, StorageError> open() noexcept;
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    [[nodiscard]] std::expected<std::int64_t, StorageError> store(
        const core::IncidentSnapshot& incident) noexcept override;
    [[nodiscard]] std::expected<std::vector<StoredIncidentSummary>, StorageError>
    list(std::size_t maximum_results = 1'000U) const noexcept;
    [[nodiscard]] std::expected<StoredIncidentPage, StorageError> list_page(
        const IncidentListQuery& query) const noexcept override;
    [[nodiscard]] std::expected<std::shared_ptr<const core::IncidentSnapshot>, StorageError>
    load(std::int64_t incident_id) const noexcept override;
    [[nodiscard]] std::expected<IncidentAnnotation, StorageError> annotation(
        std::int64_t incident_id) const noexcept override;
    [[nodiscard]] std::expected<void, StorageError> update_annotation(
        std::int64_t incident_id, const IncidentAnnotation& annotation) noexcept override;
    [[nodiscard]] std::expected<void, StorageError> update_annotation_with_origin(
        std::int64_t incident_id, const IncidentAnnotation& annotation,
        ClassificationChangeOrigin origin) noexcept;
    [[nodiscard]] std::expected<std::vector<IncidentClassificationHistoryEntry>,
                                StorageError>
    classification_history(std::int64_t incident_id) const noexcept;
    [[nodiscard]] std::expected<std::optional<std::int64_t>, StorageError>
    incident_id_for_export_key(const IncidentExportKey& key) const noexcept;
    [[nodiscard]] std::expected<ProcessProfileContext, StorageError>
    process_profile_context(std::int64_t incident_id,
                            std::span<const std::string> executable_keys) const noexcept override;
    [[nodiscard]] std::expected<void, StorageError>
    store_process_profile_updates(
        std::int64_t incident_id,
        std::span<const ProcessProfileUpdate> updates) noexcept override;
    [[nodiscard]] std::expected<ProcessProfileStorageStatistics, StorageError>
    process_profile_storage_statistics() const noexcept;
    [[nodiscard]] std::expected<FeedbackCalibrationContext, StorageError>
    feedback_calibration_context(
        std::int64_t incident_id,
        std::size_t maximum_observations =
            maximum_feedback_calibration_observations) const noexcept override;
    [[nodiscard]] std::expected<FeedbackProfileControlState, StorageError>
    reset_feedback_profile() noexcept override;
    [[nodiscard]] std::expected<FeedbackProfileControlState, StorageError>
    rollback_feedback_profile_reset() noexcept override;
    [[nodiscard]] std::expected<ContributorFeedbackContext, StorageError>
    contributor_feedback_context(
        std::int64_t incident_id,
        std::span<const std::string> executable_keys,
        std::size_t maximum_observations =
            maximum_contributor_feedback_observations) const noexcept override;
    [[nodiscard]] std::expected<void, StorageError>
    update_contributor_feedback(
        std::int64_t incident_id, std::string executable_key,
        ContributorFeedbackResource resource,
        ContributorFeedbackDisposition disposition,
        ContributorFeedbackTemporalRelationship temporal_relationship) noexcept override;
    [[nodiscard]] std::expected<std::vector<StoredRecurringIncident>, StorageError>
    recurring_incidents(std::size_t maximum_results = maximum_recurring_incidents)
        const noexcept override;
    [[nodiscard]] std::expected<std::string, StorageError>
    recurring_group_override(std::int64_t incident_id) const noexcept override;
    [[nodiscard]] std::expected<void, StorageError> store_incident_features(
        std::span<const StoredIncidentFeatureCache> features) noexcept override;
    [[nodiscard]] std::expected<void, StorageError>
    update_recurring_group_override(std::int64_t incident_id,
                                    std::string override_group) noexcept override;
    [[nodiscard]] std::expected<std::uint64_t, StorageError> incident_count() const noexcept;
    [[nodiscard]] std::expected<std::int32_t, StorageError> schema_version() const noexcept;
    [[nodiscard]] std::expected<std::uint64_t, StorageError> database_size_bytes() const noexcept;
    [[nodiscard]] std::expected<void, StorageError>
    backup_to(const std::filesystem::path& destination) const noexcept;
    // Restore is explicit and first creates a verified safety backup. Both paths
    // must not exist as the current archive; existing destination files are refused.
    [[nodiscard]] std::expected<void, StorageError>
    restore_from(const std::filesystem::path& source,
                 const std::filesystem::path& safety_backup) noexcept;
    // Maintenance is caller-initiated and serialized with archive reads/writes. Normal
    // recording never invokes it and never deletes incidents automatically.
    [[nodiscard]] std::expected<ArchiveMaintenanceResult, StorageError>
    apply_retention(const ArchiveRetentionPolicy& policy) noexcept;
    [[nodiscard]] std::expected<ArchiveMaintenanceResult, StorageError>
    purge_all_incidents() noexcept;

    [[nodiscard]] const ArchiveConfiguration& configuration() const noexcept;

private:
    struct NativeState;
    ArchiveConfiguration configuration_{};
    std::unique_ptr<NativeState> native_{};
};

} // namespace blackbox::storage
