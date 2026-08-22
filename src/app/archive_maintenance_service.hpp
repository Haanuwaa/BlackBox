#pragma once

#include "storage/incident_archive.hpp"
#include "storage/incident_writer.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace blackbox::app {

struct ArchiveMaintenanceSnapshot {
    bool busy{};
    bool healthy{};
    bool recoverable_incident{};
    std::uint64_t recoverable_capture_sequence{};
    std::uint64_t incident_count{};
    std::uint64_t database_size_bytes{};
    std::uint64_t maximum_bytes{};
    std::int32_t schema_version{};
    std::string archive_path{};
    std::string status{"Waiting for archive health check"};
    std::uint64_t generation{};
};

class ArchiveMaintenanceService final {
public:
    ArchiveMaintenanceService(storage::SqliteIncidentArchive& archive,
                              storage::IncidentWriter& writer) noexcept;
    ~ArchiveMaintenanceService();
    void start();
    void stop() noexcept;
    void refresh();
    void retry_failed();
    void backup(std::filesystem::path destination);
    void restore(std::filesystem::path source,
                 std::filesystem::path safety_backup);
    void retain_newest(std::size_t maximum_incidents);
    void export_dataset(std::filesystem::path destination);
    void export_failed(std::filesystem::path destination);
    void purge_all();
    [[nodiscard]] std::shared_ptr<const ArchiveMaintenanceSnapshot> snapshot() const;

private:
    enum class JobType : std::uint8_t {
        refresh, retry, backup, restore, retention, export_dataset,
        export_failed, purge
    };
    struct Job {
        JobType type{JobType::refresh};
        std::filesystem::path first{};
        std::filesystem::path second{};
        std::size_t maximum_incidents{};
    };
    void enqueue(Job job);
    void run(std::stop_token stop_token) noexcept;
    void execute(const Job& job);
    void publish_health(std::string status, bool operation_succeeded);

    storage::SqliteIncidentArchive& archive_;
    storage::IncidentWriter& writer_;
    mutable std::mutex mutex_{};
    std::condition_variable_any available_{};
    std::deque<Job> jobs_{};
    std::jthread worker_{};
    std::shared_ptr<const ArchiveMaintenanceSnapshot> snapshot_{
        std::make_shared<const ArchiveMaintenanceSnapshot>()};
    std::uint64_t generation_{};
};

} // namespace blackbox::app
