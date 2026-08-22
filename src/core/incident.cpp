#include "core/incident.hpp"

#include <algorithm>

namespace blackbox::core {

IncidentSnapshot::IncidentSnapshot(
    IncidentHeader header,
    std::vector<IncidentSystemSample> system_samples,
    std::vector<IncidentProcessInfo> process_metadata,
    std::vector<IncidentProcessSample> process_samples,
    std::vector<SystemEvent> system_events) noexcept
    : header_{std::move(header)},
      system_samples_{std::move(system_samples)},
      process_metadata_{std::move(process_metadata)},
      process_samples_{std::move(process_samples)},
      system_events_{std::move(system_events)} {}

const IncidentHeader& IncidentSnapshot::header() const noexcept {
    return header_;
}

std::span<const IncidentSystemSample> IncidentSnapshot::system_samples() const noexcept {
    return system_samples_;
}

std::span<const IncidentProcessInfo> IncidentSnapshot::process_metadata() const noexcept {
    return process_metadata_;
}

std::span<const IncidentProcessSample> IncidentSnapshot::process_samples() const noexcept {
    return process_samples_;
}

std::span<const SystemEvent> IncidentSnapshot::system_events() const noexcept {
    return system_events_;
}

IncidentCaptureCoordinator::IncidentCaptureCoordinator(const std::size_t queue_capacity)
    : queue_capacity_{queue_capacity} {}

void IncidentCaptureCoordinator::start_accepting() noexcept {
    const std::scoped_lock lock{mutex_};
    accepting_ = true;
}

void IncidentCaptureCoordinator::stop_accepting() noexcept {
    const std::scoped_lock lock{mutex_};
    accepting_ = false;
    if (pending_.has_value()) {
        pending_.reset();
        ++captures_cancelled_;
    }
}

IncidentCaptureRequestResult IncidentCaptureCoordinator::request(
    const MonotonicTimePoint event_time,
    const std::chrono::nanoseconds pre_window,
    const std::chrono::nanoseconds post_window,
    const IncidentCaptureTrigger trigger) noexcept {
    const std::scoped_lock lock{mutex_};
    if (!accepting_) {
        return IncidentCaptureRequestResult::stopped;
    }

    const auto requested_start = event_time - pre_window;
    const auto requested_end = event_time + post_window;
    if (pending_.has_value()) {
        pending_->requested_start = std::min(pending_->requested_start, requested_start);
        pending_->requested_end = std::max(pending_->requested_end, requested_end);
        ++pending_->trigger_count;
        if (trigger.kind == IncidentTriggerKind::automatic) {
            ++pending_->automatic_trigger_count;
            if (trigger.score >= pending_->automatic_score) {
                pending_->automatic_resource = trigger.resource;
                pending_->automatic_observed_value = trigger.observed_value;
                pending_->automatic_baseline_value = trigger.baseline_value;
                pending_->automatic_score = trigger.score;
                pending_->automatic_signal = trigger.signal;
            }
        } else {
            ++pending_->manual_trigger_count;
        }
        ++capture_requests_merged_;
        last_request_rejected_ = false;
        return IncidentCaptureRequestResult::merged;
    }

    if (reserved_slots_locked() >= queue_capacity_) {
        ++queue_rejections_;
        last_request_rejected_ = true;
        return IncidentCaptureRequestResult::queue_full;
    }

    IncidentCaptureWindow window{};
    window.sequence = next_sequence_++;
    window.event_time = event_time;
    window.requested_start = requested_start;
    window.requested_end = requested_end;
    if (trigger.kind == IncidentTriggerKind::automatic) {
        window.manual_trigger_count = 0U;
        window.automatic_trigger_count = 1U;
        window.automatic_resource = trigger.resource;
        window.automatic_observed_value = trigger.observed_value;
        window.automatic_baseline_value = trigger.baseline_value;
        window.automatic_score = trigger.score;
        window.automatic_signal = trigger.signal;
    }
    pending_ = window;
    ++captures_started_;
    last_request_rejected_ = false;
    return IncidentCaptureRequestResult::started;
}

std::optional<IncidentCaptureWindow> IncidentCaptureCoordinator::try_begin_snapshot(
    const MonotonicTimePoint observed_at) noexcept {
    const std::scoped_lock lock{mutex_};
    if (snapshot_in_progress_ || !pending_.has_value() ||
        observed_at < pending_->requested_end) {
        return std::nullopt;
    }

    auto result = pending_;
    pending_.reset();
    snapshot_in_progress_ = true;
    return result;
}

void IncidentCaptureCoordinator::finish_snapshot(
    std::shared_ptr<const IncidentSnapshot> snapshot) noexcept {
    const std::scoped_lock lock{mutex_};
    if (!snapshot_in_progress_) {
        return;
    }
    snapshot_in_progress_ = false;
    if (snapshot != nullptr && queue_.size() < queue_capacity_) {
        queue_.push_back(std::move(snapshot));
        ++incidents_completed_;
        last_request_rejected_ = false;
        work_available_.notify_one();
        return;
    }
    if (snapshot == nullptr) {
        ++snapshot_failures_;
        return;
    }
    ++queue_rejections_;
    last_request_rejected_ = true;
}

void IncidentCaptureCoordinator::cancel_pending() noexcept {
    const std::scoped_lock lock{mutex_};
    if (pending_.has_value()) {
        pending_.reset();
        ++captures_cancelled_;
    }
}

std::shared_ptr<const IncidentSnapshot> IncidentCaptureCoordinator::wait_pop(
    const std::stop_token stop_token) noexcept {
    try {
        std::unique_lock lock{mutex_};
        static_cast<void>(work_available_.wait(
            lock, stop_token, [this] { return !queue_.empty(); }));
        if (queue_.empty()) {
            return {};
        }
        auto result = std::move(queue_.front());
        queue_.pop_front();
        return result;
    } catch (...) {
        return {};
    }
}

std::shared_ptr<const IncidentSnapshot> IncidentCaptureCoordinator::try_pop() noexcept {
    const std::scoped_lock lock{mutex_};
    if (queue_.empty()) {
        return {};
    }
    auto result = std::move(queue_.front());
    queue_.pop_front();
    return result;
}

IncidentCaptureStatus IncidentCaptureCoordinator::status() const noexcept {
    const std::scoped_lock lock{mutex_};
    IncidentCaptureStatus result{};
    result.accepting = accepting_;
    result.can_request = accepting_ &&
                         (pending_.has_value() || reserved_slots_locked() < queue_capacity_);
    result.has_pending_window = pending_.has_value();
    if (pending_.has_value()) {
        result.pending_window = *pending_;
    }
    result.queue_size = queue_.size();
    result.queue_capacity = queue_capacity_;
    result.captures_started = captures_started_;
    result.capture_requests_merged = capture_requests_merged_;
    result.incidents_completed = incidents_completed_;
    result.queue_rejections = queue_rejections_;
    result.snapshot_failures = snapshot_failures_;
    result.captures_cancelled = captures_cancelled_;

    if (!accepting_) {
        result.phase = IncidentCapturePhase::stopped;
    } else if (snapshot_in_progress_) {
        result.phase = IncidentCapturePhase::constructing_snapshot;
    } else if (pending_.has_value()) {
        result.phase = IncidentCapturePhase::collecting_post_window;
    } else if (last_request_rejected_) {
        result.phase = IncidentCapturePhase::queue_full;
    } else if (!queue_.empty()) {
        result.phase = IncidentCapturePhase::queued;
    } else {
        result.phase = IncidentCapturePhase::idle;
    }
    return result;
}

std::size_t IncidentCaptureCoordinator::reserved_slots_locked() const noexcept {
    return queue_.size() + static_cast<std::size_t>(snapshot_in_progress_) +
           static_cast<std::size_t>(pending_.has_value());
}

} // namespace blackbox::core
