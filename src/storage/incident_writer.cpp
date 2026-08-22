#include "storage/incident_writer.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace blackbox::storage {

IncidentWriter::IncidentWriter(core::IIncidentWorkSource& source,
                               IIncidentArchive& archive,
                               const IncidentWriterConfiguration configuration)
    : source_{source}, archive_{archive}, configuration_{configuration} {
    constexpr auto maximum_attempt_limit = 10U;
    constexpr auto maximum_delay_limit = std::chrono::seconds{5};
    if (configuration_.maximum_attempts == 0U ||
        configuration_.maximum_attempts > maximum_attempt_limit ||
        configuration_.initial_retry_delay < std::chrono::milliseconds::zero() ||
        configuration_.maximum_retry_delay < configuration_.initial_retry_delay ||
        configuration_.maximum_retry_delay > maximum_delay_limit) {
        throw std::invalid_argument{"invalid incident-writer retry configuration"};
    }
}

IncidentWriter::~IncidentWriter() {
    stop();
}

void IncidentWriter::start() {
    const std::scoped_lock lock{lifecycle_mutex_};
    if (worker_.joinable()) {
        return;
    }
    stop_policy_.store(WriterStopPolicy::drain, std::memory_order_relaxed);
    {
        const std::scoped_lock diagnostics_lock{diagnostics_mutex_};
        diagnostics_.state = diagnostics_.consecutive_failures == 0U
                                 ? WriterState::running
                                 : WriterState::degraded;
    }
    try {
        worker_ = std::jthread{[this](const std::stop_token stop_token) {
            run(stop_token);
        }};
    } catch (...) {
        const std::scoped_lock diagnostics_lock{diagnostics_mutex_};
        diagnostics_.state = WriterState::stopped;
        throw;
    }
}

void IncidentWriter::stop(const WriterStopPolicy policy) noexcept {
    const std::scoped_lock lock{lifecycle_mutex_};
    if (!worker_.joinable()) {
        const std::scoped_lock diagnostics_lock{diagnostics_mutex_};
        diagnostics_.state = WriterState::stopped;
        return;
    }
    stop_policy_.store(policy, std::memory_order_release);
    worker_.request_stop();
    try {
        worker_.join();
    } catch (...) {
        // Application shutdown never propagates a thread join failure.
    }
    const std::scoped_lock diagnostics_lock{diagnostics_mutex_};
    diagnostics_.state = WriterState::stopped;
    diagnostics_.writing = false;
    diagnostics_.retrying = false;
    diagnostics_.current_capture_sequence = 0U;
    diagnostics_.current_attempt = 0U;
}

IncidentWriterDiagnostics IncidentWriter::diagnostics() const {
    IncidentWriterDiagnostics result{};
    {
        const std::scoped_lock lock{diagnostics_mutex_};
        result = diagnostics_;
        result.write_timing = timing_summary_locked();
    }
    {
        const std::scoped_lock lock{recovery_mutex_};
        result.recoverable_incident_available = recoverable_incident_ != nullptr;
        result.recoverable_capture_sequence = recoverable_incident_ == nullptr
            ? 0U : recoverable_incident_->header().window.sequence;
    }
    return result;
}

std::expected<std::int64_t, StorageError> IncidentWriter::retry_recoverable() noexcept {
    std::shared_ptr<const core::IncidentSnapshot> incident;
    {
        const std::scoped_lock lock{recovery_mutex_};
        incident = recoverable_incident_;
    }
    if (incident == nullptr) {
        return std::unexpected{StorageError{StorageErrorCode::invalid_data, 0,
                                            "no failed incident is available"}};
    }
    const auto stored = archive_.store(*incident);
    if (!stored) {
        const std::scoped_lock lock{diagnostics_mutex_};
        diagnostics_.last_error_code = stored.error().code;
        diagnostics_.last_native_error = stored.error().native_code;
        diagnostics_.last_error_message = stored.error().message;
        return std::unexpected{stored.error()};
    }
    {
        const std::scoped_lock lock{recovery_mutex_};
        if (recoverable_incident_ == incident) recoverable_incident_.reset();
    }
    {
        const std::scoped_lock lock{diagnostics_mutex_};
        ++diagnostics_.recoveries;
        ++diagnostics_.succeeded;
        diagnostics_.consecutive_failures = 0U;
        diagnostics_.last_stored_incident_id = *stored;
        diagnostics_.last_error_message.clear();
        diagnostics_.state = WriterState::running;
    }
    return *stored;
}

std::shared_ptr<const core::IncidentSnapshot>
IncidentWriter::recoverable_incident() const noexcept {
    const std::scoped_lock lock{recovery_mutex_};
    return recoverable_incident_;
}

void IncidentWriter::discard_recoverable() noexcept {
    const std::scoped_lock lock{recovery_mutex_};
    recoverable_incident_.reset();
}

void IncidentWriter::run(const std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
        auto incident = source_.wait_pop(stop_token);
        if (incident != nullptr) {
            process(std::move(incident));
        }
    }

    while (auto incident = source_.try_pop()) {
        if (stop_policy_.load(std::memory_order_acquire) == WriterStopPolicy::drain) {
            process(std::move(incident));
        } else {
            const std::scoped_lock lock{diagnostics_mutex_};
            ++diagnostics_.cancelled;
        }
    }
}

void IncidentWriter::process(
    std::shared_ptr<const core::IncidentSnapshot> incident) noexcept {
    if (incident == nullptr) {
        return;
    }
    for (std::uint32_t attempt = 1U; attempt <= configuration_.maximum_attempts;
         ++attempt) {
        {
            const std::scoped_lock lock{diagnostics_mutex_};
            diagnostics_.writing = true;
            diagnostics_.retrying = attempt > 1U;
            diagnostics_.current_capture_sequence = incident->header().window.sequence;
            diagnostics_.current_attempt = attempt;
            ++diagnostics_.attempts;
            if (attempt > 1U) {
                ++diagnostics_.retry_attempts;
            }
        }

        const auto started = std::chrono::steady_clock::now();
        const auto stored = archive_.store(*incident);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started);

        bool retry = false;
        {
            const std::scoped_lock lock{diagnostics_mutex_};
            record_duration(elapsed);
            diagnostics_.writing = false;
            if (stored.has_value()) {
                if (diagnostics_.consecutive_failures != 0U) {
                    ++diagnostics_.recoveries;
                }
                diagnostics_.consecutive_failures = 0U;
                diagnostics_.state = WriterState::running;
                diagnostics_.retrying = false;
                diagnostics_.current_capture_sequence = 0U;
                diagnostics_.current_attempt = 0U;
                diagnostics_.last_error_message.clear();
                ++diagnostics_.succeeded;
                diagnostics_.last_stored_incident_id = *stored;
                return;
            }

            ++diagnostics_.consecutive_failures;
            diagnostics_.state = WriterState::degraded;
            diagnostics_.last_error_code = stored.error().code;
            diagnostics_.last_native_error = stored.error().native_code;
            diagnostics_.last_error_message = stored.error().message;
            retry = is_retryable(stored.error().code) &&
                    attempt < configuration_.maximum_attempts;
            diagnostics_.retrying = retry;
            if (!retry) {
                ++diagnostics_.failed;
                if (is_retryable(stored.error().code) &&
                    attempt == configuration_.maximum_attempts) {
                    ++diagnostics_.retry_exhausted;
                }
                diagnostics_.current_capture_sequence = 0U;
                diagnostics_.current_attempt = 0U;
            }
        }
        if (!retry) {
            const std::scoped_lock recovery_lock{recovery_mutex_};
            if (recoverable_incident_ == nullptr) {
                recoverable_incident_ = std::move(incident);
            } else {
                const std::scoped_lock diagnostics_lock{diagnostics_mutex_};
                ++diagnostics_.failed_incidents_not_retained;
            }
            return;
        }
        std::this_thread::sleep_for(retry_delay(attempt));
    }
}

bool IncidentWriter::is_retryable(const StorageErrorCode code) noexcept {
    return code == StorageErrorCode::busy || code == StorageErrorCode::io;
}

std::chrono::milliseconds IncidentWriter::retry_delay(
    const std::uint32_t failed_attempt) const noexcept {
    auto delay = configuration_.initial_retry_delay;
    for (std::uint32_t index = 1U; index < failed_attempt; ++index) {
        if (delay >= configuration_.maximum_retry_delay / 2) {
            return configuration_.maximum_retry_delay;
        }
        delay *= 2;
    }
    return std::min(delay, configuration_.maximum_retry_delay);
}

void IncidentWriter::record_duration(const std::chrono::nanoseconds duration) noexcept {
    durations_[duration_next_] = duration;
    duration_next_ = (duration_next_ + 1U) % durations_.size();
    duration_size_ = std::min(duration_size_ + 1U, durations_.size());
}

WriterTimingSummary IncidentWriter::timing_summary_locked() const noexcept {
    WriterTimingSummary result{};
    result.samples = static_cast<std::uint64_t>(duration_size_);
    if (duration_size_ == 0U) {
        return result;
    }
    std::vector<std::chrono::nanoseconds> values;
    try {
        values.assign(durations_.begin(), durations_.begin() +
                                            static_cast<std::ptrdiff_t>(duration_size_));
    } catch (...) {
        return result;
    }
    std::sort(values.begin(), values.end());
    std::chrono::nanoseconds total{};
    for (const auto value : values) {
        total += value;
    }
    result.average = total / static_cast<std::int64_t>(values.size());
    const auto percentile = [&values](const std::size_t numerator) {
        const auto rank = (values.size() * numerator + 99U) / 100U;
        return values[std::max<std::size_t>(1U, rank) - 1U];
    };
    result.p95 = percentile(95U);
    result.p99 = percentile(99U);
    result.maximum = values.back();
    return result;
}

} // namespace blackbox::storage
