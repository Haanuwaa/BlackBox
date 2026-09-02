#pragma once

#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace blackbox::app {

inline constexpr std::uint32_t support_bundle_format_version = 1U;
inline constexpr std::uintmax_t maximum_support_crash_evidence_bytes = 64ULL << 20U;

struct SupportDiagnostics {
    std::string application_version{};
    std::string platform{};
    bool collector_running{};
    bool automatic_detection_enabled{};
    bool process_path_collection_enabled{};
    bool foreground_identity_enabled{};
    bool process_lifecycle_enabled{};
    bool power_and_device_events_enabled{};
    bool audio_device_events_enabled{};
    bool system_event_evidence_enabled{};
    std::uint64_t collections{};
    std::uint64_t partial_samples{};
    std::uint64_t failed_samples{};
    std::uint64_t dropped_samples{};
    std::uint64_t deadline_misses{};
    std::uint64_t resume_events{};
    std::uint64_t provider_recoveries{};
    std::uint64_t collector_worker_failures{};
    std::uint64_t incident_captures_started{};
    std::uint64_t incidents_completed{};
    std::uint64_t incident_snapshot_failures{};
    std::uint64_t incident_queue_rejections{};
    std::uint64_t automatic_detector_triggers{};
    std::uint64_t system_events_recorded{};
    std::uint64_t system_events_dropped{};
    std::uint64_t process_lifecycle_observations{};
    std::uint64_t process_lifecycle_events_recorded{};
    bool storage_enabled{};
    bool storage_writer_running{};
    bool archive_healthy{};
    std::int32_t archive_schema_version{};
    std::uint64_t stored_incidents{};
    std::uint64_t archive_database_bytes{};
    std::uint64_t archive_maximum_bytes{};
    std::uint64_t storage_write_attempts{};
    std::uint64_t storage_retry_attempts{};
    std::uint64_t storage_retry_exhausted{};
    std::uint64_t storage_write_successes{};
    std::uint64_t storage_write_failures{};
    bool recoverable_incident_available{};
    std::uint64_t previous_crash_evidence{};
    std::uint64_t renderer_frames{};
    std::uint64_t renderer_hitches{};
    std::uint64_t renderer_present_failures{};
    double renderer_frame_p95_milliseconds{};
    double renderer_frame_maximum_milliseconds{};
    std::uint64_t app_metric_payloads{};
    std::uint64_t app_diagnostic_payloads{};
    double app_cumulative_cpu_seconds{};
    double app_cumulative_gpu_seconds{};
    std::uint64_t app_hang_diagnostics{};
    double app_hang_duration_seconds{};
};

struct SupportBundleRequest {
    std::filesystem::path destination{};
    SupportDiagnostics diagnostics{};
    std::optional<std::filesystem::path> consented_crash_evidence{};
    bool crash_evidence_disclosure_confirmed{};
};

struct SupportBundleResult {
    std::filesystem::path destination{};
    std::uint64_t files{};
    std::uint64_t bytes{};
    bool included_crash_evidence{};
};

enum class SupportBundleErrorCode : std::uint8_t {
    invalid_request,
    destination_exists,
    staging_exists,
    crash_evidence_invalid,
    crash_evidence_too_large,
    cannot_write,
    cannot_publish,
};

struct SupportBundleError {
    SupportBundleErrorCode code{SupportBundleErrorCode::invalid_request};
    std::string message{};
    friend bool operator==(const SupportBundleError&, const SupportBundleError&) = default;
};

[[nodiscard]] std::expected<SupportBundleResult, SupportBundleError>
create_support_bundle(const SupportBundleRequest& request) noexcept;

struct SupportBundleServiceSnapshot {
    bool running{};
    bool busy{};
    std::string status{"Support bundle service is stopped"};
    std::uint64_t generation{};
};

class SupportBundleService final {
public:
    SupportBundleService() = default;
    ~SupportBundleService();
    SupportBundleService(const SupportBundleService&) = delete;
    SupportBundleService& operator=(const SupportBundleService&) = delete;
    SupportBundleService(SupportBundleService&&) = delete;
    SupportBundleService& operator=(SupportBundleService&&) = delete;

    void start();
    void stop() noexcept;
    void create(SupportBundleRequest request);
    [[nodiscard]] std::shared_ptr<const SupportBundleServiceSnapshot> snapshot() const;

private:
    void run(std::stop_token stop_token) noexcept;
    void publish(bool busy, std::string status);

    mutable std::mutex mutex_{};
    std::condition_variable_any available_{};
    std::optional<SupportBundleRequest> pending_{};
    std::jthread worker_{};
    std::shared_ptr<const SupportBundleServiceSnapshot> snapshot_{
        std::make_shared<const SupportBundleServiceSnapshot>()};
    std::uint64_t generation_{};
};

} // namespace blackbox::app
