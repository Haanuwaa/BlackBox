#include "app/incident_viewer_service.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace blackbox::app {

IncidentViewerService::IncidentViewerService(
    storage::IIncidentRepository& repository, analysis::IIncidentAnalyzer* analyzer,
    storage::IProcessProfileRepository* profile_repository,
    storage::IFeedbackCalibrationRepository* feedback_repository,
    storage::IRecurringIncidentRepository* recurring_repository) noexcept
    : repository_{repository}, analyzer_{analyzer}, profile_repository_{profile_repository},
      feedback_repository_{feedback_repository}, recurring_repository_{recurring_repository} {}

IncidentViewerService::~IncidentViewerService() { stop(); }

void IncidentViewerService::start() {
    const std::scoped_lock lock{mutex_};
    if (worker_.joinable()) return;
    accepting_jobs_ = true;
    worker_ = std::jthread{[this](const std::stop_token stop_token) { run(stop_token); }};
}

void IncidentViewerService::stop() noexcept {
    std::jthread worker;
    {
        const std::scoped_lock lock{mutex_};
        accepting_jobs_ = false;
        queue_diagnostics_.cancelled_reads += read_jobs_.size();
        read_jobs_.clear();
        queue_diagnostics_.queued_reads = 0U;
        if (!worker_.joinable()) return;
        worker_.request_stop();
        available_.notify_all();
        worker = std::move(worker_);
    }
    if (worker.joinable()) worker.join();
}

void IncidentViewerService::request_page(const std::size_t offset, std::string search,
                                         const ui::IncidentListOrder order) {
    Job job{};
    job.type = JobType::page;
    job.offset = offset;
    job.search = std::move(search);
    job.order = order;
    enqueue(std::move(job));
}

void IncidentViewerService::request_detail(const std::int64_t incident_id) {
    Job job{};
    job.type = JobType::detail;
    job.incident_id = incident_id;
    enqueue(std::move(job));
}

void IncidentViewerService::request_process(const std::int64_t incident_id,
                                            const core::IncidentProcessIdentity identity) {
    Job job{};
    job.type = JobType::process;
    job.incident_id = incident_id;
    job.identity = identity;
    enqueue(std::move(job));
}

bool IncidentViewerService::update_annotation(const std::int64_t incident_id, std::string label,
                                              std::string note,
                                              const storage::IncidentUserFeedback feedback,
                                              const storage::IncidentCategory category) {
    Job job{};
    job.type = JobType::annotation;
    job.incident_id = incident_id;
    job.label = std::move(label);
    job.note = std::move(note);
    job.feedback = feedback;
    job.category = category;
    return enqueue(std::move(job));
}

bool IncidentViewerService::update_contributor_feedback(
    const std::int64_t incident_id, std::string executable_key,
    const storage::ContributorFeedbackResource resource,
    const storage::ContributorFeedbackDisposition disposition,
    const storage::ContributorFeedbackTemporalRelationship temporal_relationship) {
    Job job{};
    job.type = JobType::contributor_feedback;
    job.incident_id = incident_id;
    job.contributor_executable_key = std::move(executable_key);
    job.contributor_resource = resource;
    job.contributor_disposition = disposition;
    job.contributor_temporal_relationship = temporal_relationship;
    return enqueue(std::move(job));
}

void IncidentViewerService::request_recurring_incidents() {
    Job job{};
    job.type = JobType::recurring;
    enqueue(std::move(job));
}

bool IncidentViewerService::update_recurring_group_override(const std::int64_t incident_id,
                                                            std::string override_group) {
    Job job{};
    job.type = JobType::recurring_override;
    job.incident_id = incident_id;
    job.recurring_group_override = std::move(override_group);
    return enqueue(std::move(job));
}

bool IncidentViewerService::reset_feedback_profile() {
    Job job{};
    job.type = JobType::feedback_reset;
    return enqueue(std::move(job));
}

bool IncidentViewerService::rollback_feedback_profile_reset() {
    Job job{};
    job.type = JobType::feedback_rollback;
    return enqueue(std::move(job));
}

std::shared_ptr<const ui::IncidentViewerContent> IncidentViewerService::snapshot() const {
    const std::scoped_lock lock{mutex_};
    return snapshot_;
}

IncidentViewerQueueDiagnostics IncidentViewerService::queue_diagnostics() const noexcept {
    const std::scoped_lock lock{mutex_};
    return queue_diagnostics_;
}

bool IncidentViewerService::is_mutation(const JobType type) noexcept {
    return type == JobType::annotation || type == JobType::contributor_feedback ||
           type == JobType::recurring_override || type == JobType::feedback_reset ||
           type == JobType::feedback_rollback;
}

bool IncidentViewerService::enqueue(Job job) {
    constexpr std::size_t maximum_reads = 8U;
    constexpr std::size_t maximum_mutations = 64U;
    const bool mutation = is_mutation(job.type);
    bool rejected = false;
    {
        const std::scoped_lock lock{mutex_};
        if (!accepting_jobs_) {
            if (mutation) {
                ++queue_diagnostics_.rejected_mutations;
                rejected = true;
            } else {
                ++queue_diagnostics_.cancelled_reads;
            }
        } else if (mutation) {
            if (mutation_jobs_.size() == maximum_mutations) {
                ++queue_diagnostics_.rejected_mutations;
                rejected = true;
            } else {
                mutation_jobs_.push_back(std::move(job));
                queue_diagnostics_.queued_mutations = mutation_jobs_.size();
            }
        } else {
            const auto same_type = [type = job.type](const Job& queued) {
                return queued.type == type;
            };
            const auto previous = std::find_if(read_jobs_.begin(), read_jobs_.end(), same_type);
            if (previous != read_jobs_.end()) {
                *previous = std::move(job);
                ++queue_diagnostics_.coalesced_reads;
            } else {
                if (read_jobs_.size() == maximum_reads) {
                    read_jobs_.pop_front();
                    ++queue_diagnostics_.cancelled_reads;
                }
                read_jobs_.push_back(std::move(job));
            }
            queue_diagnostics_.queued_reads = read_jobs_.size();
        }
        if (!rejected) available_.notify_one();
    }
    if (rejected) {
        publish_error("Incident change was not queued because the viewer is stopping or "
                      "its mutation queue is full; retry the change");
    }
    return !rejected;
}

void IncidentViewerService::run(const std::stop_token stop_token) noexcept {
    for (;;) {
        Job job{};
        bool mutation = false;
        {
            std::unique_lock lock{mutex_};
            static_cast<void>(available_.wait(lock, stop_token, [this] {
                return !mutation_jobs_.empty() || !read_jobs_.empty();
            }));
            if (!mutation_jobs_.empty()) {
                job = std::move(mutation_jobs_.front());
                mutation_jobs_.pop_front();
                queue_diagnostics_.queued_mutations = mutation_jobs_.size();
                mutation = true;
            } else if (stop_token.stop_requested()) {
                break;
            } else if (!read_jobs_.empty()) {
                job = std::move(read_jobs_.front());
                read_jobs_.pop_front();
                queue_diagnostics_.queued_reads = read_jobs_.size();
            } else {
                continue;
            }
        }
        bool mutation_succeeded = true;
        try {
            switch (job.type) {
            case JobType::page:
                handle_page(job);
                break;
            case JobType::detail:
                handle_detail(job);
                break;
            case JobType::process:
                handle_process(job);
                break;
            case JobType::annotation:
                mutation_succeeded = handle_annotation(job);
                break;
            case JobType::contributor_feedback:
                mutation_succeeded = handle_contributor_feedback(job);
                break;
            case JobType::recurring:
                handle_recurring();
                break;
            case JobType::recurring_override:
                mutation_succeeded = handle_recurring_override(job);
                break;
            case JobType::feedback_reset:
                mutation_succeeded = handle_feedback_reset();
                break;
            case JobType::feedback_rollback:
                mutation_succeeded = handle_feedback_rollback();
                break;
            }
        } catch (const std::exception& exception) {
            mutation_succeeded = false;
            publish_error(exception.what());
        } catch (...) {
            mutation_succeeded = false;
            publish_error("Unknown incident viewer failure");
        }
        const std::scoped_lock lock{mutex_};
        if (mutation) {
            if (mutation_succeeded) {
                ++queue_diagnostics_.completed_mutations;
            } else {
                ++queue_diagnostics_.failed_mutations;
            }
        } else {
            ++queue_diagnostics_.completed_reads;
        }
    }
}

} // namespace blackbox::app
