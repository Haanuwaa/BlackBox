#include "app/incident_viewer_service.hpp"

#include <algorithm>
#include <chrono>
#include <string>

namespace blackbox::app {
namespace {

[[nodiscard]] double milliseconds(const std::chrono::steady_clock::duration value) noexcept {
    return std::chrono::duration<double, std::milli>{value}.count();
}

[[nodiscard]] std::string note_preview(const std::string& note) {
    constexpr std::size_t maximum = 96U;
    if (note.size() <= maximum) return note;
    return note.substr(0U, maximum - 3U) + "...";
}

} // namespace

bool IncidentViewerService::handle_annotation(const Job& job) {
    const storage::IncidentAnnotation annotation{job.label, job.note, job.feedback, job.category};
    const auto started = std::chrono::steady_clock::now();
    auto updated = repository_.update_annotation(job.incident_id, annotation);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (!updated) {
        publish_error(updated.error().message);
        return false;
    }
    loaded_annotation_ = annotation;
    for (auto& summary : last_page_.incidents) {
        if (summary.id == job.incident_id) {
            summary.label = job.label;
            summary.note = job.note;
        }
    }
    auto content = *snapshot();
    for (auto& row : content.incidents) {
        if (row.id == job.incident_id) {
            row.label = job.label;
            row.note_preview = note_preview(job.note);
        }
    }
    if (content.detail && content.detail->id == job.incident_id) {
        content.detail->label = job.label;
        content.detail->note = job.note;
        content.detail->user_feedback = static_cast<ui::IncidentFeedback>(job.feedback);
        content.detail->category = static_cast<ui::IncidentCategory>(job.category);
    }
    content.state = ui::IncidentViewerLoadState::ready;
    content.status = "Incident details saved";
    content.last_query_milliseconds = milliseconds(elapsed);
    publish(std::move(content));
    return true;
}

bool IncidentViewerService::handle_contributor_feedback(const Job& job) {
    if (feedback_repository_ == nullptr) {
        publish_error("Contributor feedback storage is unavailable");
        return false;
    }
    const auto updated = feedback_repository_->update_contributor_feedback(
        job.incident_id, job.contributor_executable_key, job.contributor_resource,
        job.contributor_disposition, job.contributor_temporal_relationship);
    if (!updated) {
        publish_error(updated.error().message);
        return false;
    }
    handle_detail(Job{.type = JobType::detail, .incident_id = job.incident_id});
    auto content = *snapshot();
    content.state = ui::IncidentViewerLoadState::ready;
    content.status =
        job.contributor_disposition == storage::ContributorFeedbackDisposition::unsure
            ? "Contributor attribution cleared"
        : job.contributor_disposition ==
                    storage::ContributorFeedbackDisposition::confirmed_contributor &&
                job.contributor_temporal_relationship !=
                    storage::ContributorFeedbackTemporalRelationship::preceding_activity
            ? "Attribution saved for this incident; non-preceding confirmation "
              "cannot teach positive uplift"
            : "Contributor attribution saved for future exact matches";
    publish(std::move(content));
    return true;
}

bool IncidentViewerService::handle_feedback_reset() {
    if (feedback_repository_ == nullptr) {
        publish_error("Feedback profile storage is unavailable");
        return false;
    }
    const auto reset = feedback_repository_->reset_feedback_profile();
    if (!reset) {
        publish_error(reset.error().message);
        return false;
    }
    if (loaded_incident_id_ != 0) {
        handle_detail(Job{.type = JobType::detail, .incident_id = loaded_incident_id_});
    }
    auto content = *snapshot();
    content.state = ui::IncidentViewerLoadState::ready;
    content.status = "Feedback influence reset; prior annotations remain stored";
    publish(std::move(content));
    return true;
}

bool IncidentViewerService::handle_feedback_rollback() {
    if (feedback_repository_ == nullptr) {
        publish_error("Feedback profile storage is unavailable");
        return false;
    }
    const auto rolled_back = feedback_repository_->rollback_feedback_profile_reset();
    if (!rolled_back) {
        publish_error(rolled_back.error().message);
        return false;
    }
    if (loaded_incident_id_ != 0) {
        handle_detail(Job{.type = JobType::detail, .incident_id = loaded_incident_id_});
    }
    auto content = *snapshot();
    content.state = ui::IncidentViewerLoadState::ready;
    content.status = "Feedback influence reset rolled back";
    publish(std::move(content));
    return true;
}

bool IncidentViewerService::handle_recurring_override(const Job& job) {
    if (recurring_repository_ == nullptr) {
        auto content = *snapshot();
        content.recurring.state = ui::RecurringIncidentViewState::error;
        content.recurring.status = "Recurring storage unavailable";
        publish(std::move(content));
        return false;
    }
    const auto updated = recurring_repository_->update_recurring_group_override(
        job.incident_id, job.recurring_group_override);
    if (!updated) {
        auto content = *snapshot();
        content.recurring.state = ui::RecurringIncidentViewState::error;
        content.recurring.status = updated.error().message;
        publish(std::move(content));
        return false;
    }
    auto content = *snapshot();
    if (content.detail && content.detail->id == job.incident_id)
        content.detail->recurring_group_override = job.recurring_group_override;
    content.status = job.recurring_group_override.empty()
                         ? "Returned incident to automatic recurring grouping"
                         : "Recurring group override saved";
    publish(std::move(content));
    handle_recurring();
    return true;
}



} // namespace blackbox::app
