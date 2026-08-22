#pragma once

#include "core/incident.hpp"
#include "storage/incident_archive.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace blackbox::storage {

enum class WriterState : std::uint8_t {
    stopped,
    running,
    degraded,
};

enum class WriterStopPolicy : std::uint8_t {
    drain,
    cancel,
};

struct WriterTimingSummary {
    std::uint64_t samples{};
    std::chrono::nanoseconds average{};
    std::chrono::nanoseconds p95{};
    std::chrono::nanoseconds p99{};
    std::chrono::nanoseconds maximum{};
    friend constexpr bool operator==(const WriterTimingSummary&,
                                     const WriterTimingSummary&) = default;
};

struct IncidentWriterConfiguration {
    std::uint32_t maximum_attempts{3U};
    std::chrono::milliseconds initial_retry_delay{25};
    std::chrono::milliseconds maximum_retry_delay{100};
    friend constexpr bool operator==(const IncidentWriterConfiguration&,
                                     const IncidentWriterConfiguration&) = default;
};

struct IncidentWriterDiagnostics {
    WriterState state{WriterState::stopped};
    bool writing{};
    bool retrying{};
    std::uint64_t attempts{};
    std::uint64_t retry_attempts{};
    std::uint64_t retry_exhausted{};
    std::uint64_t succeeded{};
    std::uint64_t failed{};
    std::uint64_t recoveries{};
    std::uint64_t consecutive_failures{};
    std::uint64_t cancelled{};
    bool recoverable_incident_available{};
    std::uint64_t recoverable_capture_sequence{};
    std::uint64_t failed_incidents_not_retained{};
    std::uint64_t current_capture_sequence{};
    std::uint32_t current_attempt{};
    std::int64_t last_stored_incident_id{};
    StorageErrorCode last_error_code{StorageErrorCode::sql_error};
    int last_native_error{};
    std::string last_error_message{};
    WriterTimingSummary write_timing{};
};

class IncidentWriter final {
public:
    IncidentWriter(core::IIncidentWorkSource& source,
                   IIncidentArchive& archive,
                   IncidentWriterConfiguration configuration = {});
    ~IncidentWriter();

    IncidentWriter(const IncidentWriter&) = delete;
    IncidentWriter& operator=(const IncidentWriter&) = delete;

    void start();
    void stop(WriterStopPolicy policy = WriterStopPolicy::drain) noexcept;
    [[nodiscard]] IncidentWriterDiagnostics diagnostics() const;
    [[nodiscard]] std::expected<std::int64_t, StorageError>
    retry_recoverable() noexcept;
    [[nodiscard]] std::shared_ptr<const core::IncidentSnapshot>
    recoverable_incident() const noexcept;
    void discard_recoverable() noexcept;

private:
    void run(std::stop_token stop_token) noexcept;
    void process(std::shared_ptr<const core::IncidentSnapshot> incident) noexcept;
    [[nodiscard]] static bool is_retryable(StorageErrorCode code) noexcept;
    [[nodiscard]] std::chrono::milliseconds retry_delay(
        std::uint32_t failed_attempt) const noexcept;
    void record_duration(std::chrono::nanoseconds duration) noexcept;
    [[nodiscard]] WriterTimingSummary timing_summary_locked() const noexcept;

    core::IIncidentWorkSource& source_;
    IIncidentArchive& archive_;
    IncidentWriterConfiguration configuration_{};
    mutable std::mutex lifecycle_mutex_{};
    std::jthread worker_{};
    std::atomic<WriterStopPolicy> stop_policy_{WriterStopPolicy::drain};

    mutable std::mutex diagnostics_mutex_{};
    IncidentWriterDiagnostics diagnostics_{};
    std::array<std::chrono::nanoseconds, 256U> durations_{};
    std::size_t duration_size_{};
    std::size_t duration_next_{};
    mutable std::mutex recovery_mutex_{};
    std::shared_ptr<const core::IncidentSnapshot> recoverable_incident_{};
};

} // namespace blackbox::storage
