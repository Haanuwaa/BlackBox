#include "app/archive_maintenance_service.hpp"

#include "storage/incident_dataset.hpp"

#include <utility>

namespace blackbox::app {

ArchiveMaintenanceService::ArchiveMaintenanceService(
    storage::SqliteIncidentArchive& archive, storage::IncidentWriter& writer) noexcept
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
    Job job{JobType::backup}; job.first = std::move(path); enqueue(std::move(job));
}
void ArchiveMaintenanceService::restore(std::filesystem::path source,
                                        std::filesystem::path safety) {
    Job job{JobType::restore};
    job.first = std::move(source); job.second = std::move(safety);
    enqueue(std::move(job));
}
void ArchiveMaintenanceService::retain_newest(const std::size_t count) {
    Job job{JobType::retention}; job.maximum_incidents = count; enqueue(std::move(job));
}
void ArchiveMaintenanceService::export_dataset(std::filesystem::path path) {
    Job job{JobType::export_dataset}; job.first = std::move(path); enqueue(std::move(job));
}
void ArchiveMaintenanceService::export_failed(std::filesystem::path path) {
    Job job{JobType::export_failed}; job.first = std::move(path); enqueue(std::move(job));
}
void ArchiveMaintenanceService::purge_all() { enqueue({JobType::purge}); }

std::shared_ptr<const ArchiveMaintenanceSnapshot>
ArchiveMaintenanceService::snapshot() const {
    const std::scoped_lock lock{mutex_};
    return snapshot_;
}

void ArchiveMaintenanceService::enqueue(Job job) {
    const std::scoped_lock lock{mutex_};
    // Maintenance is user initiated and bounded. A newer health refresh supersedes
    // an older pending refresh; destructive jobs are never silently discarded.
    if (job.type == JobType::refresh) {
        for (const auto& pending : jobs_) if (pending.type == JobType::refresh) return;
    }
    if (jobs_.size() >= 16U) {
        auto state = std::make_shared<ArchiveMaintenanceSnapshot>(*snapshot_);
        state->status = "Maintenance queue is full; wait for the current action";
        state->generation = ++generation_;
        snapshot_ = std::move(state);
        return;
    }
    jobs_.push_back(std::move(job));
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
        try { execute(job); }
        catch (const std::exception& exception) { publish_health(exception.what(), false); }
        catch (...) { publish_health("Unknown archive maintenance failure", false); }
    }
}

void ArchiveMaintenanceService::execute(const Job& job) {
    std::string status;
    bool succeeded = true;
    switch (job.type) {
    case JobType::refresh: status = "Archive health refreshed"; break;
    case JobType::retry: {
        const auto result = writer_.retry_recoverable();
        succeeded = result.has_value();
        status = succeeded ? "Failed incident recovered as archive incident " +
                                 std::to_string(*result)
                           : "Retry failed: " + result.error().message;
        break;
    }
    case JobType::backup: {
        const auto result = archive_.backup_to(job.first);
        succeeded = result.has_value();
        status = succeeded ? "Verified backup created at " + job.first.string()
                           : "Backup failed: " + result.error().message;
        break;
    }
    case JobType::restore: {
        const auto result = archive_.restore_from(job.first, job.second);
        succeeded = result.has_value();
        status = succeeded ? "Archive restored; pre-restore safety backup: " +
                                 job.second.string()
                           : "Restore refused or failed: " + result.error().message;
        break;
    }
    case JobType::retention: {
        storage::ArchiveRetentionPolicy policy{};
        policy.maximum_incidents = job.maximum_incidents;
        policy.compact_after_delete = true;
        const auto result = archive_.apply_retention(policy);
        succeeded = result.has_value();
        status = succeeded ? "Retention deleted " +
                                 std::to_string(result->incidents_deleted) + " incidents"
                           : "Retention failed: " + result.error().message;
        break;
    }
    case JobType::export_dataset: {
        const auto result = storage::export_incident_dataset(archive_, job.first);
        succeeded = result.has_value();
        status = succeeded ? "Evidence dataset exported with " +
                                 std::to_string(result->incidents) + " incidents"
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
        storage::SqliteIncidentArchive destination{{job.first}};
        const auto opened = destination.open();
        const auto stored = opened ? destination.store(*incident)
                                   : std::expected<std::int64_t, storage::StorageError>{
                                         std::unexpected{opened.error()}};
        destination.close();
        succeeded = stored.has_value();
        if (!succeeded) {
            std::error_code ignored;
            std::filesystem::remove(job.first, ignored);
        }
        status = succeeded ? "Recoverable incident exported to " + job.first.string()
                           : "Failed-incident export failed: " + stored.error().message;
        break;
    }
    case JobType::purge: {
        const auto result = archive_.purge_all_incidents();
        succeeded = result.has_value();
        status = succeeded ? "Privacy purge removed " +
                                 std::to_string(result->incidents_deleted) + " incidents"
                           : "Purge failed: " + result.error().message;
        break;
    }
    }
    publish_health(std::move(status), succeeded);
}

void ArchiveMaintenanceService::publish_health(std::string status,
                                               const bool) {
    auto state = std::make_shared<ArchiveMaintenanceSnapshot>();
    state->archive_path = archive_.configuration().path.string();
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
    snapshot_ = std::move(state);
}

} // namespace blackbox::app
