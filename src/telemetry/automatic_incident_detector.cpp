#include "telemetry/automatic_incident_detector.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace blackbox::telemetry {
namespace {

[[nodiscard]] bool valid_metric(const DetectorMetricConfiguration& metric) noexcept {
    return std::isfinite(metric.hard_threshold) && metric.hard_threshold > 0.0 &&
           std::isfinite(metric.statistical_floor) && metric.statistical_floor >= 0.0 &&
           metric.statistical_floor <= metric.hard_threshold &&
           std::isfinite(metric.minimum_standard_deviation) &&
           metric.minimum_standard_deviation > 0.0;
}

} // namespace

std::expected<AutomaticDetectorConfiguration, AutomaticDetectorConfigurationError>
validate_automatic_detector_configuration(
    const AutomaticDetectorConfiguration configuration) noexcept {
    if (!valid_metric(configuration.cpu) || !valid_metric(configuration.memory) ||
        !valid_metric(configuration.disk) || !valid_metric(configuration.network)) {
        return std::unexpected{
            AutomaticDetectorConfigurationError::invalid_metric_threshold};
    }
    if (configuration.baseline_samples < 2U ||
        configuration.baseline_samples > 60U) {
        return std::unexpected{
            AutomaticDetectorConfigurationError::invalid_baseline_samples};
    }
    if (configuration.consecutive_samples == 0U) {
        return std::unexpected{
            AutomaticDetectorConfigurationError::invalid_consecutive_samples};
    }
    if (!std::isfinite(configuration.statistical_z_score) ||
        configuration.statistical_z_score <= 0.0) {
        return std::unexpected{AutomaticDetectorConfigurationError::invalid_z_score};
    }
    if (configuration.cooldown < std::chrono::nanoseconds::zero()) {
        return std::unexpected{AutomaticDetectorConfigurationError::negative_cooldown};
    }
    if (!std::isfinite(configuration.disk_stall_seconds) ||
        configuration.disk_stall_seconds <= 0.0 ||
        !std::isfinite(configuration.disk_queue_depth) ||
        configuration.disk_queue_depth <= 0.0 ||
        !std::isfinite(configuration.network_retransmit_fraction) ||
        configuration.network_retransmit_fraction <= 0.0 ||
        configuration.network_retransmit_fraction > 1.0 ||
        configuration.network_failure_events == 0U) {
        return std::unexpected{
            AutomaticDetectorConfigurationError::invalid_event_threshold};
    }
    return configuration;
}

AutomaticIncidentDetector::AutomaticIncidentDetector(
    const AutomaticDetectorConfiguration configuration) {
    const auto validated = validate_automatic_detector_configuration(configuration);
    if (!validated) {
        throw std::invalid_argument{"invalid automatic detector configuration"};
    }
    configuration_ = *validated;
}

std::optional<core::IncidentCaptureTrigger> AutomaticIncidentDetector::observe(
    const SystemSample& sample) noexcept {
    ++diagnostics_.samples_observed;
    std::array<Candidate, 4U> candidates{};

    if (!configuration_.cpu_enabled) {
        metrics_[0] = {};
    } else if (sample.cpu_usage.has_value()) {
        candidates[0] = evaluate(core::AutomaticIncidentResource::cpu,
                                 sample.cpu_usage.value.value,
                                 configuration_.cpu, metrics_[0]);
    } else {
        observe_unavailable(metrics_[0]);
    }
    if (!configuration_.memory_enabled) {
        metrics_[1] = {};
    } else if (sample.memory_usage.has_value()) {
        candidates[1] = evaluate(core::AutomaticIncidentResource::memory,
                                 sample.memory_usage.value.value,
                                 configuration_.memory, metrics_[1]);
    } else {
        observe_unavailable(metrics_[1]);
    }

    const auto sum_rates = [this](const auto& left, const auto& right,
                                  MetricState& state) -> std::optional<double> {
        if (!left.has_value() && !right.has_value()) {
            observe_unavailable(state);
            return std::nullopt;
        }
        return (left.has_value() ? left.value.value : 0.0) +
               (right.has_value() ? right.value.value : 0.0);
    };
    if (!configuration_.disk_enabled) {
        metrics_[2] = {};
    } else if (const auto disk = sum_rates(sample.disk_read_rate, sample.disk_write_rate,
                                           metrics_[2])) {
        candidates[2] = evaluate(core::AutomaticIncidentResource::disk, *disk,
                                 configuration_.disk, metrics_[2]);
    }
    if (configuration_.disk_enabled) {
        Candidate quality{};
        quality.resource = core::AutomaticIncidentResource::disk;
        quality.single_observation = true;
        if (sample.disk_service_time.has_value() &&
            sample.disk_service_time.value.value >= configuration_.disk_stall_seconds) {
            quality.observed = sample.disk_service_time.value.value;
            quality.baseline = configuration_.disk_stall_seconds;
            quality.score = quality.observed / quality.baseline;
            quality.ready = true;
            quality.signal = core::AutomaticIncidentSignal::disk_latency;
        }
        if (sample.disk_queue_depth.has_value() &&
            sample.disk_queue_depth.value >= configuration_.disk_queue_depth) {
            const auto score = sample.disk_queue_depth.value /
                               configuration_.disk_queue_depth;
            if (!quality.ready || score > quality.score) {
                quality.observed = sample.disk_queue_depth.value;
                quality.baseline = configuration_.disk_queue_depth;
                quality.score = score;
                quality.ready = true;
                quality.signal = core::AutomaticIncidentSignal::disk_queue_depth;
            }
        }
        if (quality.ready && (!candidates[2].ready || quality.score > candidates[2].score)) {
            candidates[2] = quality;
        }
    }
    if (!configuration_.network_enabled) {
        metrics_[3] = {};
    } else if (const auto network = sum_rates(sample.network_receive_rate,
                                              sample.network_transmit_rate, metrics_[3])) {
        candidates[3] = evaluate(core::AutomaticIncidentResource::network, *network,
                                 configuration_.network, metrics_[3]);
    }
    if (configuration_.network_enabled) {
        Candidate quality{};
        quality.resource = core::AutomaticIncidentResource::network;
        quality.single_observation = true;
        const auto consider = [&quality](const double observed, const double threshold,
                                         const core::AutomaticIncidentSignal signal) {
            const auto score = observed / threshold;
            if (!quality.ready || score > quality.score) {
                quality.observed = observed;
                quality.baseline = threshold;
                quality.score = score;
                quality.ready = true;
                quality.signal = signal;
            }
        };
        if (sample.network_connectivity.has_value()) {
            const auto level = sample.network_connectivity.value;
            if (level == NetworkConnectivityLevel::disconnected) {
                consider(1.0, 1.0,
                         core::AutomaticIncidentSignal::network_connectivity);
            } else if (level == NetworkConnectivityLevel::constrained &&
                       sample.network_interface_changes.has_value() &&
                       sample.network_interface_changes.value != 0U) {
                consider(1.0, 1.0,
                         core::AutomaticIncidentSignal::network_interface_transition);
            }
        }
        if (sample.network_tcp_retransmit_fraction.has_value() &&
            sample.network_tcp_retransmit_fraction.value.value >=
                configuration_.network_retransmit_fraction) {
            consider(sample.network_tcp_retransmit_fraction.value.value,
                     configuration_.network_retransmit_fraction,
                     core::AutomaticIncidentSignal::tcp_retransmission);
        }
        if (sample.network_tcp_failed_connections.has_value() &&
            sample.network_tcp_failed_connections.value >=
                configuration_.network_failure_events) {
            consider(static_cast<double>(sample.network_tcp_failed_connections.value),
                     static_cast<double>(configuration_.network_failure_events),
                     core::AutomaticIncidentSignal::tcp_connection_failure);
        }
        if (sample.network_tcp_resets.has_value() &&
            sample.network_tcp_resets.value >= configuration_.network_failure_events) {
            consider(static_cast<double>(sample.network_tcp_resets.value),
                     static_cast<double>(configuration_.network_failure_events),
                     core::AutomaticIncidentSignal::tcp_connection_reset);
        }
        if (quality.ready && (!candidates[3].ready || quality.score > candidates[3].score)) {
            candidates[3] = quality;
        }
    }

    const Candidate* strongest = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.ready && (strongest == nullptr || candidate.score > strongest->score)) {
            strongest = &candidate;
        }
    }
    if (strongest == nullptr) {
        return std::nullopt;
    }
    if (last_trigger_at_.has_value() && sample.observed_at >= *last_trigger_at_ &&
        sample.observed_at - *last_trigger_at_ < configuration_.cooldown) {
        ++diagnostics_.triggers_suppressed_by_cooldown;
        return std::nullopt;
    }

    last_trigger_at_ = sample.observed_at;
    ++diagnostics_.triggers_emitted;
    if (strongest->single_observation) {
        ++diagnostics_.single_observation_triggers;
    }
    return core::IncidentCaptureTrigger{
        core::IncidentTriggerKind::automatic, strongest->resource,
        strongest->observed, strongest->baseline, strongest->score,
        strongest->signal};
}

void AutomaticIncidentDetector::reset() noexcept {
    metrics_ = {};
    last_trigger_at_.reset();
}

std::expected<void, AutomaticDetectorConfigurationError>
AutomaticIncidentDetector::reconfigure(
    const AutomaticDetectorConfiguration configuration) noexcept {
    const auto validated = validate_automatic_detector_configuration(configuration);
    if (!validated) {
        return std::unexpected{validated.error()};
    }
    configuration_ = *validated;
    reset();
    return {};
}

AutomaticDetectorDiagnostics AutomaticIncidentDetector::diagnostics() const noexcept {
    return diagnostics_;
}

const AutomaticDetectorConfiguration& AutomaticIncidentDetector::configuration() const noexcept {
    return configuration_;
}

AutomaticIncidentDetector::Candidate AutomaticIncidentDetector::evaluate(
    const core::AutomaticIncidentResource resource,
    const double value,
    const DetectorMetricConfiguration& configuration,
    MetricState& state) noexcept {
    Candidate candidate{};
    candidate.resource = resource;
    candidate.observed = value;
    if (!std::isfinite(value) || value < 0.0) {
        observe_unavailable(state);
        return candidate;
    }

    const auto baseline_ready = state.size >= configuration_.baseline_samples;
    const auto mean = state.size == 0U ? 0.0 : state.sum / static_cast<double>(state.size);
    const auto variance = state.size < 2U
                              ? 0.0
                              : std::max(0.0, state.sum_squares /
                                                 static_cast<double>(state.size) - mean * mean);
    const auto deviation = std::max(std::sqrt(variance),
                                    configuration.minimum_standard_deviation);
    const auto z_score = baseline_ready ? (value - mean) / deviation : 0.0;
    const auto hard = value >= configuration.hard_threshold;
    const auto statistical = baseline_ready && value >= configuration.statistical_floor &&
                             z_score >= configuration_.statistical_z_score;
    const auto anomalous = hard || statistical;
    state.consecutive = anomalous ? state.consecutive + 1U : 0U;
    candidate.baseline = mean;
    candidate.score = std::max(value / configuration.hard_threshold,
                               z_score / configuration_.statistical_z_score);
    candidate.ready = anomalous &&
                      state.consecutive >= configuration_.consecutive_samples;

    // Do not teach an active anomaly to the baseline. This also gives a hard
    // event a deterministic consecutive-sample delay independent of its size.
    if (!anomalous) {
        if (state.size < configuration_.baseline_samples) {
            state.values[state.next] = value;
            state.sum += value;
            state.sum_squares += value * value;
            ++state.size;
            state.next = (state.next + 1U) % configuration_.baseline_samples;
        } else {
            const auto removed = state.values[state.next];
            state.values[state.next] = value;
            state.sum += value - removed;
            state.sum_squares += value * value - removed * removed;
            state.next = (state.next + 1U) % configuration_.baseline_samples;
        }
    }
    return candidate;
}

void AutomaticIncidentDetector::observe_unavailable(MetricState& state) noexcept {
    ++diagnostics_.unavailable_metric_values;
    state.consecutive = 0U;
}

} // namespace blackbox::telemetry
