#include "app/archive_maintenance_service.hpp"

#include "storage/incident_dataset.hpp"
#include "app/incident_viewer_service.hpp"
#include "telemetry/collector.hpp"
#include "core/filesystem_text.hpp"

#include <utility>

namespace blackbox::app {

ArchiveMaintenanceService::ArchiveMaintenanceService(storage::SqliteIncidentArchive& archive,
                                                     storage::IncidentWriter& writer) noexcept
    : archive_{archive}, writer_{writer} {}

ArchiveMaintenanceService::~ArchiveMaintenanceService() { stop(); }

void ArchiveMaintenanceService::start() {
    const std::scoped_lock lock{mutex_};
    if (worker_.joinable()) return;
    worker_ = std::jthread{[this](const std::stop_token token) { run(token); }};
}

void ArchiveMaintenanceService::stop() noexcept {
    std::jthread worker;
    {
        const std::scoped_lock lock{mutex_};
        if (!worker_.joinable()) return;
        worker_.request_stop();
        available_.notify_all();
        worker = std::move(worker_);
    }
    if (worker.joinable()) worker.join();
}

void ArchiveMaintenanceService::refresh() { enqueue({JobType::refresh}); }
void ArchiveMaintenanceService::retry_failed() { enqueue({JobType::retry}); }
void ArchiveMaintenanceService::backup(std::filesystem::path path) {
    Job job{JobType::backup};
    job.first = std::move(path);
    enqueue(std::move(job));
}
void ArchiveMaintenanceService::restore(std::filesystem::path source,
                                        std::filesystem::path safety) {
    Job job{JobType::restore};
    job.first = std::move(source);
    job.second = std::move(safety);
    enqueue(std::move(job));
}
void ArchiveMaintenanceService::retain_newest(const std::size_t count) {
    Job job{JobType::retention};
    job.maximum_incidents = count;
    enqueue(std::move(job));
}
void ArchiveMaintenanceService::export_dataset(std::filesystem::path path) {
    Job job{JobType::export_dataset};
    job.first = std::move(path);
    enqueue(std::move(job));
}
void ArchiveMaintenanceService::export_failed(std::filesystem::path path) {
    Job job{JobType::export_failed};
    job.first = std::move(path);
    enqueue(std::move(job));
}
void ArchiveMaintenanceService::purge_all() { enqueue({JobType::purge}); }

void ArchiveMaintenanceService::attach_lifecycle(telemetry::TelemetryCollector* collector,
                                                 telemetry::SystemEventCollector* events,
                                                 IncidentViewerService* viewer) noexcept {
    collector_ = collector;
    events_ = events;
    viewer_ = viewer;
}

std::shared_ptr<const ArchiveMaintenanceSnapshot> ArchiveMaintenanceService::snapshot() const {
    const std::scoped_lock lock{mutex_};
    return snapshot_;
}

void ArchiveMaintenanceService::enqueue(Job job) {
    const std::scoped_lock lock{mutex_};
    // Maintenance is user initiated and bounded. A newer health refresh supersedes
    // an older pending refresh; destructive jobs are never silently discarded.
    if (job.type == JobType::refresh) {
        for (const auto& pending : jobs_)
            if (pending.type == JobType::refresh) return;
    }
    if (jobs_.size() >= 16U) {
        auto state = std::make_shared<ArchiveMaintenanceSnapshot>(*snapshot_);
        state->status = "Maintenance queue is full; wait for the current action";
        state->generation = ++generation_;
        snapshot_ = std::move(state);
        return;
    }
    jobs_.push_back(std::move(job));
    if (jobs_.back().type == JobType::purge || jobs_.back().type == JobType::restore)
        boundaries_pending_.fetch_add(1);
    available_.notify_one();
}

void ArchiveMaintenanceService::run(const std::stop_token token) noexcept {
    while (!token.stop_requested()) {
        Job job{};
        {
            std::unique_lock lock{mutex_};
            static_cast<void>(available_.wait(lock, token, [this] { return !jobs_.empty(); }));
            if (jobs_.empty()) continue;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            auto state = std::make_shared<ArchiveMaintenanceSnapshot>(*snapshot_);
            state->busy = true;
            state->status = "Archive maintenance in progress";
            state->generation = ++generation_;
            snapshot_ = std::move(state);
        }
        try {
            execute(job);
        } catch (const std::exception& exception) {
            publish_health(exception.what(), false);
        } catch (...) {
            publish_health("Unknown archive maintenance failure", false);
        }
    }
}

void ArchiveMaintenanceService::execute(const Job& job) {
    if (job.type != JobType::purge && job.type != JobType::restore) {
        execute_job(job);
        return;
    }
    const bool restart_collector = collector_ != nullptr && collector_->running();
    const bool restart_events = events_ != nullptr && events_->running();
    const bool restart_writer = writer_.diagnostics().state != storage::WriterState::stopped;
    struct BoundaryCompletion {
        std::atomic<unsigned>& count;
        ~BoundaryCompletion() { count.fetch_sub(1); }
    } completion{boundaries_pending_};
    const auto resume = [&] {
        if (restart_writer) writer_.start();
        if (viewer_ != nullptr) viewer_->start();
        if (restart_collector) collector_->start();
        if (restart_events) events_->start();
    };
    try {
        if (events_ != nullptr) events_->stop();
        if (collector_ != nullptr) collector_->stop();
        writer_.stop(job.type == JobType::purge ? storage::WriterStopPolicy::cancel
                                                : storage::WriterStopPolicy::drain);
        if (job.type == JobType::restore && writer_.recoverable_incident() != nullptr) {
            publish_health("Restore paused: retry or export the failed incident, then purge it "
                           "before restoring",
                           false);
        } else {
            if (viewer_ != nullptr) viewer_->stop();
            // No producer or downstream reader can now republish the old epoch.
            if (collector_ != nullptr)
                collector_->reconfigure(*telemetry::validate_recorder_configuration(
                    collector_->diagnostics().configuration));
            if (events_ != nullptr) events_->reconfigure(events_->diagnostics().configuration);
            if (viewer_ != nullptr) viewer_->invalidate_archive();
            ++content_epoch_;
            if (job.type == JobType::purge) writer_.discard_recoverable();
            execute_job(job);
        }
    } catch (...) {
        resume();
        throw;
    }
    resume();
}

void ArchiveMaintenanceService::execute_job(const Job& job) {
    std::string status;
    bool succeeded = true;
    switch (job.type) {
    case JobType::refresh:
        status = "Archive health refreshed";
        break;
    case JobType::retry: {
        const auto result = writer_.retry_recoverable();
        succeeded = result.has_value();
        status = succeeded
                     ? "Failed incident recovered as archive incident " + std::to_string(*result)
                     : "Retry failed: " + result.error().message;
        break;
    }
    case JobType::backup: {
        const auto result = archive_.backup_to(job.first);
        succeeded = result.has_value();
        status = succeeded ? "Verified backup created at " + core::path_to_utf8(job.first)
                           : "Backup failed: " + result.error().message;
        break;
    }
    case JobType::restore: {
        const auto result = archive_.restore_from(job.first, job.second);
        succeeded = result.has_value();
        status = succeeded ? "Archive restored; pre-restore safety backup: " +
                                 core::path_to_utf8(job.second)
                           : "Restore refused or failed: " + result.error().message;
        break;
    }
    case JobType::retention: {
        storage::ArchiveRetentionPolicy policy{};
        policy.maximum_incidents = job.maximum_incidents;
        policy.compact_after_delete = true;
        const auto result = archive_.apply_retention(policy);
        succeeded = result.has_value();
        status = succeeded ? "Retention deleted " + std::to_string(result->incidents_deleted) +
                                 " incidents"
                           : "Retention failed: " + result.error().message;
        break;
    }
    case JobType::export_dataset: {
        const auto result = storage::export_incident_dataset(archive_, job.first);
        succeeded = result.has_value();
        status = succeeded ? "Evidence dataset exported with " + std::to_string(result->incidents) +
                                 " incidents"
                           : "Export failed: " + result.error().message;
        break;
    }
    case JobType::export_failed: {
        const auto incident = writer_.recoverable_incident();
        if (incident == nullptr || job.first.empty() || std::filesystem::exists(job.first)) {
            succeeded = false;
            status = "Failed-incident export requires a new destination file";
            break;
        }
        // Build privately, then use the verified, exclusive publication path.
        // A destination created concurrently must never be overwritten or removed.
        storage::SqliteIncidentArchive destination{{":memory:"}};
        const auto opened = destination.open();
        const auto stored = opened ? destination.store(*incident)
                                   : std::expected<std::int64_t, storage::StorageError>{
                                         std::unexpected{opened.error()}};
        const auto exported =
            stored ? destination.backup_to(job.first)
                   : std::expected<void, storage::StorageError>{std::unexpected{stored.error()}};
        destination.close();
        succeeded = exported.has_value();
        status = succeeded ? "Recoverable incident exported to " + core::path_to_utf8(job.first)
                           : "Failed-incident export failed: " + exported.error().message;
        break;
    }
    case JobType::purge: {
        const auto result = archive_.purge_all_incidents();
        succeeded = result.has_value();
        status = succeeded ? "Privacy purge removed " + std::to_string(result->incidents_deleted) +
                                 " incidents"
                           : "Purge failed: " + result.error().message;
        break;
    }
    }
    publish_health(std::move(status), succeeded);
}

void ArchiveMaintenanceService::publish_health(std::string status, const bool) {
    auto state = std::make_shared<ArchiveMaintenanceSnapshot>();
    state->archive_path = core::path_to_utf8(archive_.configuration().path);
    state->maximum_bytes = archive_.configuration().maximum_bytes;
    const auto count = archive_.incident_count();
    const auto size = archive_.database_size_bytes();
    const auto schema = archive_.schema_version();
    const auto writer = writer_.diagnostics();
    state->healthy = count.has_value() && size.has_value() && schema.has_value() &&
                     *schema == storage::current_schema_version;
    if (count) state->incident_count = *count;
    if (size) state->database_size_bytes = *size;
    if (schema) state->schema_version = *schema;
    state->recoverable_incident = writer.recoverable_incident_available;
    state->recoverable_capture_sequence = writer.recoverable_capture_sequence;
    state->status = std::move(status);
    state->busy = false;
    const std::scoped_lock lock{mutex_};
    state->generation = ++generation_;
    state->content_epoch = content_epoch_;
    snapshot_ = std::move(state);
}

} // namespace blackbox::app
