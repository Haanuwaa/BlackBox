#pragma once

#include "storage/incident_archive.hpp"
#include "ui/incident_viewer.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace blackbox::analysis {
class IIncidentAnalyzer;
}

namespace blackbox::app {

struct IncidentViewerQueueDiagnostics {
    std::size_t queued_reads{};
    std::size_t queued_mutations{};
    std::uint64_t coalesced_reads{};
    std::uint64_t cancelled_reads{};
    std::uint64_t rejected_mutations{};
    std::uint64_t completed_reads{};
    std::uint64_t completed_mutations{};
    std::uint64_t failed_mutations{};
};

class IncidentViewerService final {
public:
    explicit IncidentViewerService(
        storage::IIncidentRepository& repository, analysis::IIncidentAnalyzer* analyzer = nullptr,
        storage::IProcessProfileRepository* profile_repository = nullptr,
        storage::IFeedbackCalibrationRepository* feedback_repository = nullptr,
        storage::IRecurringIncidentRepository* recurring_repository = nullptr) noexcept;
    ~IncidentViewerService();

    IncidentViewerService(const IncidentViewerService&) = delete;
    IncidentViewerService& operator=(const IncidentViewerService&) = delete;

    void start();
    void stop() noexcept;
    // Requires a stopped worker; used by the archive replacement boundary.
    void invalidate_archive();
    void request_page(std::size_t offset, std::string search, ui::IncidentListOrder order);
    void request_detail(std::int64_t incident_id);
    void request_process(std::int64_t incident_id, core::IncidentProcessIdentity identity);
    bool update_annotation(std::int64_t incident_id, std::string label, std::string note,
                           storage::IncidentUserFeedback feedback,
                           storage::IncidentCategory category);
    bool update_contributor_feedback(
        std::int64_t incident_id, std::string executable_key,
        storage::ContributorFeedbackResource resource,
        storage::ContributorFeedbackDisposition disposition,
        storage::ContributorFeedbackTemporalRelationship temporal_relationship);
    void request_recurring_incidents();
    bool update_recurring_group_override(std::int64_t incident_id, std::string override_group);
    bool reset_feedback_profile();
    bool rollback_feedback_profile_reset();
    bool export_summary(std::shared_ptr<const ui::IncidentViewerContent> content,
                        std::filesystem::path destination, bool include_annotations = false);
    [[nodiscard]] std::shared_ptr<const ui::IncidentViewerContent> snapshot() const;
    [[nodiscard]] IncidentViewerQueueDiagnostics queue_diagnostics() const noexcept;

private:
    enum class JobType : std::uint8_t {
        page,
        detail,
        process,
        annotation,
        contributor_feedback,
        recurring,
        recurring_override,
        feedback_reset,
        feedback_rollback,
        summary_export
    };
    struct Job {
        JobType type{JobType::page};
        std::size_t offset{};
        std::string search{};
        ui::IncidentListOrder order{ui::IncidentListOrder::newest_first};
        std::int64_t incident_id{};
        core::IncidentProcessIdentity identity{};
        std::string label{};
        std::string note{};
        storage::IncidentUserFeedback feedback{storage::IncidentUserFeedback::unanswered};
        storage::IncidentCategory category{storage::IncidentCategory::unknown};
        std::string contributor_executable_key{};
        storage::ContributorFeedbackResource contributor_resource{
            storage::ContributorFeedbackResource::cpu};
        storage::ContributorFeedbackDisposition contributor_disposition{
            storage::ContributorFeedbackDisposition::unsure};
        storage::ContributorFeedbackTemporalRelationship contributor_temporal_relationship{
            storage::ContributorFeedbackTemporalRelationship::preceding_activity};
        std::string recurring_group_override{};
        std::shared_ptr<const ui::IncidentViewerContent> export_content{};
        std::filesystem::path destination{};
        bool include_annotations{};
    };

    bool enqueue(Job job);
    [[nodiscard]] static bool is_mutation(JobType type) noexcept;
    void run(std::stop_token stop_token) noexcept;
    void handle_page(const Job& job);
    void handle_detail(const Job& job);
    void handle_process(const Job& job);
    [[nodiscard]] bool handle_annotation(const Job& job);
    [[nodiscard]] bool handle_contributor_feedback(const Job& job);
    void handle_recurring();
    [[nodiscard]] bool handle_recurring_override(const Job& job);
    [[nodiscard]] bool handle_feedback_reset();
    [[nodiscard]] bool handle_feedback_rollback();
    [[nodiscard]] bool handle_summary_export(const Job& job);
    void publish(ui::IncidentViewerContent content);
    void publish_error(std::string message);
    [[nodiscard]] storage::IncidentListQuery storage_query(const Job& job) const;

    storage::IIncidentRepository& repository_;
    analysis::IIncidentAnalyzer* analyzer_{};
    storage::IProcessProfileRepository* profile_repository_{};
    storage::IFeedbackCalibrationRepository* feedback_repository_{};
    storage::IRecurringIncidentRepository* recurring_repository_{};
    mutable std::mutex mutex_{};
    std::condition_variable_any available_{};
    std::deque<Job> read_jobs_{};
    std::deque<Job> mutation_jobs_{};
    std::jthread worker_{};
    std::shared_ptr<const ui::IncidentViewerContent> snapshot_{
        std::make_shared<const ui::IncidentViewerContent>()};
    storage::StoredIncidentPage last_page_{};
    storage::IncidentListQuery last_query_{};
    std::shared_ptr<const core::IncidentSnapshot> loaded_incident_{};
    storage::IncidentAnnotation loaded_annotation_{};
    ui::IncidentAnalysisView loaded_analysis_{};
    std::int64_t loaded_incident_id_{};
    std::int64_t loaded_created_utc_milliseconds_{};
    std::map<std::int64_t, std::int64_t> recurring_created_utc_by_id_{};
    IncidentViewerQueueDiagnostics queue_diagnostics_{};
    bool accepting_jobs_{};
    std::uint64_t generation_{};
};

} // namespace blackbox::app
