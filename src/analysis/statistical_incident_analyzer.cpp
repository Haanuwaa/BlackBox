#include "analysis/statistical_incident_analyzer.hpp"

#include "analysis/contributor_ranker.hpp"
#include "analysis/robust_baseline.hpp"
#include "analysis/workload_context.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

namespace blackbox::analysis {
namespace {

[[nodiscard]] bool available(const core::RecordedValue<double>& value) noexcept {
    return value.status == core::RecordedValueStatus::available &&
           std::isfinite(value.value);
}

[[nodiscard]] bool available(const core::RecordedValue<std::uint64_t>& value) noexcept {
    return value.status == core::RecordedValueStatus::available;
}

[[nodiscard]] bool available(const core::RecordedValue<std::uint8_t>& value) noexcept {
    return value.status == core::RecordedValueStatus::available;
}

[[nodiscard]] double numeric(const core::RecordedValue<double>& value) noexcept {
    return value.value;
}

[[nodiscard]] double numeric(const core::RecordedValue<std::uint64_t>& value) noexcept {
    return static_cast<double>(value.value);
}

[[nodiscard]] double numeric(const core::RecordedValue<std::uint8_t>& value) noexcept {
    // Stored connectivity is the portable NetworkConnectivityLevel encoding.
    // Analyze it as disruption severity so worse states always point in the
    // same (higher) anomaly direction: internet, unknown, local, constrained,
    // disconnected. Unknown is weak evidence, not a fabricated outage.
    switch (value.value) {
    case 2U: return 0.0; // internet
    case 4U: return 1.0; // unknown
    case 1U: return 2.0; // local only
    case 3U: return 3.0; // constrained
    case 0U: return 4.0; // disconnected
    default: return 1.0;
    }
}

class MetricAccumulator final {
public:
    MetricAccumulator(const MetricKind metric, const std::size_t capacity,
                      const std::size_t minimum_samples)
        : baseline_{capacity}, minimum_samples_{minimum_samples} {
        evidence_.metric = metric;
    }

    template <typename T>
    void add_baseline(const core::RecordedValue<T>& value) noexcept {
        if (available(value)) {
            baseline_.add(numeric(value));
        } else {
            ++evidence_.missing_baseline_samples;
        }
    }

    void prepare() {
        evidence_.baseline = baseline_.summarize();
        evidence_.availability = evidence_.baseline.sample_count >= minimum_samples_
                                     ? EvidenceAvailability::available
                                     : EvidenceAvailability::insufficient_baseline;
    }

    template <typename T>
    void evaluate(const core::RecordedValue<T>& value,
                  const core::MonotonicTimePoint observed_at) {
        if (!available(value)) {
            ++evidence_.missing_evaluation_samples;
            return;
        }
        ++evidence_.evaluation_samples;
        const auto observed = numeric(value);
        if (evidence_.availability != EvidenceAvailability::available) {
            if (!has_observation_ || observed > evidence_.observed_value) {
                has_observation_ = true;
                evidence_.observed_value = observed;
                evidence_.observed_at = observed_at;
                evidence_.direction = AnomalyDirection::higher;
            }
            return;
        }
        const auto z = robust_z_score(evidence_.baseline, observed);
        const auto score = anomaly_score(std::abs(z));
        if (!has_observation_ || score > evidence_.score ||
            (score == evidence_.score && std::abs(z) > std::abs(evidence_.robust_z))) {
            has_observation_ = true;
            evidence_.score = score;
            evidence_.robust_z = z;
            evidence_.baseline_percentile = baseline_percentile(baseline_, observed);
            evidence_.observed_value = observed;
            evidence_.observed_at = observed_at;
            evidence_.direction = z > 0.0 ? AnomalyDirection::higher
                                          : z < 0.0 ? AnomalyDirection::lower
                                                    : AnomalyDirection::unchanged;
        }
    }

    void finish() noexcept {
        if (evidence_.availability == EvidenceAvailability::available &&
            evidence_.evaluation_samples == 0U) {
            evidence_.availability = EvidenceAvailability::unavailable;
        }
    }

    [[nodiscard]] const MetricAnomalyEvidence& evidence() const noexcept {
        return evidence_;
    }

private:
    RollingRobustBaseline baseline_;
    std::size_t minimum_samples_{};
    MetricAnomalyEvidence evidence_{};
    bool has_observation_{};
};

[[nodiscard]] AnalysisConfidence confidence_for(
    const std::vector<MetricAnomalyEvidence>& evidence) noexcept {
    const MetricAnomalyEvidence* strongest = nullptr;
    bool has_insufficient = false;
    for (const auto& item : evidence) {
        has_insufficient = has_insufficient ||
                           item.availability == EvidenceAvailability::insufficient_baseline;
        if (item.availability == EvidenceAvailability::available &&
            (strongest == nullptr || item.score > strongest->score)) {
            strongest = &item;
        }
    }
    if (strongest == nullptr) {
        return has_insufficient ? AnalysisConfidence::low
                                : AnalysisConfidence::unavailable;
    }
    if (strongest->baseline.sample_count >= 30U &&
        strongest->evaluation_samples >= 3U) {
        return AnalysisConfidence::high;
    }
    return AnalysisConfidence::moderate;
}

[[nodiscard]] double maximum_score(
    const std::vector<MetricAnomalyEvidence>& evidence) noexcept {
    double result{};
    for (const auto& item : evidence) {
        result = (std::max)(result, item.score);
        result = (std::max)(result, cold_start_process_activity_score(item));
    }
    return result;
}

[[nodiscard]] bool at_least(const double value, const double minimum) noexcept {
    return std::isfinite(value) && value >= minimum;
}

[[nodiscard]] bool practical_resource_effect(
    const MetricAnomalyEvidence& evidence,
    const ResourceEffectFloorConfiguration& floors) noexcept {
    if (evidence.availability != EvidenceAvailability::available ||
        evidence.direction != AnomalyDirection::higher || evidence.score <= 0.0 ||
        !std::isfinite(evidence.observed_value) ||
        !std::isfinite(evidence.baseline.median)) {
        return false;
    }
    const auto increase = evidence.observed_value - evidence.baseline.median;
    switch (evidence.metric) {
    case MetricKind::system_cpu:
        return at_least(evidence.observed_value, floors.cpu_minimum_value) &&
               at_least(increase, floors.cpu_minimum_increase);
    case MetricKind::system_memory:
        return at_least(evidence.observed_value, floors.memory_minimum_value) &&
               at_least(increase, floors.memory_minimum_increase);
    case MetricKind::disk_read:
    case MetricKind::disk_write:
        return at_least(evidence.observed_value,
                        floors.disk_throughput_minimum_bytes_per_second) &&
               at_least(increase, floors.disk_throughput_minimum_increase);
    case MetricKind::disk_read_latency:
    case MetricKind::disk_write_latency:
    case MetricKind::disk_service_time:
        return at_least(evidence.observed_value,
                        floors.disk_latency_minimum_seconds) &&
               at_least(increase, floors.disk_latency_minimum_increase_seconds);
    case MetricKind::disk_queue_depth:
        return at_least(evidence.observed_value, floors.disk_queue_minimum_depth) &&
               at_least(increase, floors.disk_queue_minimum_increase);
    case MetricKind::network_receive:
    case MetricKind::network_transmit:
        return at_least(evidence.observed_value,
                        floors.network_throughput_minimum_bytes_per_second) &&
               at_least(increase, floors.network_throughput_minimum_increase);
    case MetricKind::network_connectivity:
        return at_least(evidence.observed_value,
                        floors.network_connectivity_minimum_severity) &&
               at_least(increase, 1.0);
    case MetricKind::network_tcp_retransmit:
        return at_least(evidence.observed_value,
                        floors.network_retransmit_minimum_fraction) &&
               at_least(increase, floors.network_retransmit_minimum_increase);
    case MetricKind::network_interface_changes:
    case MetricKind::network_tcp_failures:
    case MetricKind::network_tcp_resets:
        return at_least(evidence.observed_value,
                        floors.network_quality_counter_minimum) &&
               at_least(increase, floors.network_quality_counter_minimum);
    default: return false;
    }
}

struct PracticalResourceScore {
    double score{};
    std::optional<MetricKind> metric{};
};

[[nodiscard]] PracticalResourceScore maximum_practical_resource_score(
    const std::vector<MetricAnomalyEvidence>& evidence,
    const ResourceEffectFloorConfiguration& floors) noexcept {
    PracticalResourceScore result{};
    for (const auto& item : evidence) {
        if (practical_resource_effect(item, floors) &&
            (!result.metric || item.score > result.score ||
             (item.score == result.score && item.metric < *result.metric))) {
            result.score = item.score;
            result.metric = item.metric;
        }
    }
    return result;
}

struct CandidatePeaks {
    std::array<double, 4U> values{};
};

[[nodiscard]] std::set<core::IncidentProcessIdentity> select_process_candidates(
    const core::IncidentSnapshot& incident,
    const core::MonotonicTimePoint evaluation_start,
    const core::MonotonicTimePoint evaluation_end,
    const std::size_t maximum_candidates) {
    std::map<core::IncidentProcessIdentity, CandidatePeaks> peaks;
    for (const auto& sample : incident.process_samples()) {
        if (sample.observed_at < evaluation_start || sample.observed_at > evaluation_end) continue;
        auto& values = peaks[sample.identity].values;
        if (available(sample.cpu_fraction))
            values[0] = (std::max)(values[0], numeric(sample.cpu_fraction));
        if (available(sample.working_set_bytes))
            values[1] = (std::max)(values[1], numeric(sample.working_set_bytes));
        if (available(sample.disk_read_bytes_per_second))
            values[2] = (std::max)(values[2], numeric(sample.disk_read_bytes_per_second));
        if (available(sample.disk_write_bytes_per_second))
            values[3] = (std::max)(values[3], numeric(sample.disk_write_bytes_per_second));
    }

    std::set<core::IncidentProcessIdentity> selected;
    if (peaks.size() <= maximum_candidates) {
        for (const auto& [identity, unused] : peaks) {
            static_cast<void>(unused);
            selected.insert(identity);
        }
        return selected;
    }

    const auto per_metric = (std::max<std::size_t>)(1U, maximum_candidates / 4U);
    for (std::size_t metric = 0U; metric < 4U; ++metric) {
        std::vector<std::pair<double, core::IncidentProcessIdentity>> ranked;
        ranked.reserve(peaks.size());
        for (const auto& [identity, values] : peaks) {
            ranked.emplace_back(values.values[metric], identity);
        }
        std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
            if (left.first != right.first) return left.first > right.first;
            return left.second < right.second;
        });
        for (std::size_t index = 0U;
             index < (std::min)(per_metric, ranked.size()); ++index) {
            selected.insert(ranked[index].second);
        }
    }
    for (const auto& [identity, unused] : peaks) {
        static_cast<void>(unused);
        if (selected.size() >= maximum_candidates) break;
        selected.insert(identity);
    }
    return selected;
}

struct ProcessWork {
    ProcessWork(const core::IncidentProcessIdentity identity,
                const StatisticalAnalysisConfiguration& configuration)
        : identity{identity},
          cpu{MetricKind::process_cpu, configuration.baseline_capacity,
              configuration.minimum_baseline_samples},
          memory{MetricKind::process_working_set, configuration.baseline_capacity,
                 configuration.minimum_baseline_samples},
          read{MetricKind::process_disk_read, configuration.baseline_capacity,
               configuration.minimum_baseline_samples},
          write{MetricKind::process_disk_write, configuration.baseline_capacity,
                configuration.minimum_baseline_samples} {}

    core::IncidentProcessIdentity identity{};
    MetricAccumulator cpu;
    MetricAccumulator memory;
    MetricAccumulator read;
    MetricAccumulator write;
};

[[nodiscard]] ProcessWork* find_work(
    std::vector<ProcessWork>& work,
    const core::IncidentProcessIdentity identity) noexcept {
    const auto found = std::lower_bound(
        work.begin(), work.end(), identity,
        [](const ProcessWork& value, const core::IncidentProcessIdentity key) {
            return value.identity < key;
        });
    return found != work.end() && found->identity == identity ? &*found : nullptr;
}

[[nodiscard]] std::string process_name(
    const core::IncidentSnapshot& incident,
    const core::IncidentProcessIdentity identity) {
    const auto found = std::find_if(
        incident.process_metadata().begin(), incident.process_metadata().end(),
        [identity](const core::IncidentProcessInfo& value) {
            return value.identity == identity;
        });
    if (found != incident.process_metadata().end() &&
        found->name.status == core::RecordedValueStatus::available) {
        return found->name.value;
    }
    return "PID " + std::to_string(identity.pid);
}

[[nodiscard]] ResourceAnomaly resource(
    const ResourceKind kind,
    std::initializer_list<const MetricAccumulator*> accumulators,
    const ResourceEffectFloorConfiguration& floors) {
    ResourceAnomaly result{};
    result.resource = kind;
    result.evidence.reserve(accumulators.size());
    for (const auto* accumulator : accumulators) {
        result.evidence.push_back(accumulator->evidence());
    }
    result.statistical_score = maximum_score(result.evidence);
    const auto practical = maximum_practical_resource_score(result.evidence, floors);
    result.score = practical.score;
    result.pressure_metric = practical.metric;
    result.uncontextualized_score = result.score;
    result.confidence = confidence_for(result.evidence);
    return result;
}

} // namespace

std::expected<StatisticalAnalysisConfiguration,
              StatisticalAnalysisConfigurationError>
validate_statistical_analysis_configuration(
    const StatisticalAnalysisConfiguration configuration) noexcept {
    if (configuration.baseline_duration <= std::chrono::nanoseconds::zero())
        return std::unexpected{
            StatisticalAnalysisConfigurationError::baseline_duration_not_positive};
    if (configuration.evaluation_pre_window < std::chrono::nanoseconds::zero())
        return std::unexpected{
            StatisticalAnalysisConfigurationError::evaluation_pre_window_negative};
    if (configuration.baseline_capacity == 0U)
        return std::unexpected{
            StatisticalAnalysisConfigurationError::baseline_capacity_zero};
    if (configuration.minimum_baseline_samples == 0U ||
        configuration.minimum_baseline_samples > configuration.baseline_capacity)
        return std::unexpected{
            StatisticalAnalysisConfigurationError::minimum_baseline_samples_invalid};
    if (configuration.maximum_process_candidates == 0U)
        return std::unexpected{
            StatisticalAnalysisConfigurationError::process_candidate_limit_zero};
    if (configuration.maximum_ranked_processes == 0U ||
        configuration.maximum_ranked_processes > configuration.maximum_process_candidates)
        return std::unexpected{
            StatisticalAnalysisConfigurationError::ranked_process_limit_invalid};
    const auto& floors = configuration.resource_effect_floors;
    const auto valid_fraction = [](const double value) noexcept {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    };
    const auto valid_nonnegative = [](const double value) noexcept {
        return std::isfinite(value) && value >= 0.0;
    };
    if (!valid_fraction(floors.cpu_minimum_value) ||
        !valid_fraction(floors.cpu_minimum_increase) ||
        !valid_fraction(floors.memory_minimum_value) ||
        !valid_fraction(floors.memory_minimum_increase) ||
        !valid_nonnegative(floors.disk_throughput_minimum_bytes_per_second) ||
        !valid_nonnegative(floors.disk_throughput_minimum_increase) ||
        !valid_nonnegative(floors.disk_latency_minimum_seconds) ||
        !valid_nonnegative(floors.disk_latency_minimum_increase_seconds) ||
        !valid_nonnegative(floors.disk_queue_minimum_depth) ||
        !valid_nonnegative(floors.disk_queue_minimum_increase) ||
        !valid_nonnegative(floors.network_throughput_minimum_bytes_per_second) ||
        !valid_nonnegative(floors.network_throughput_minimum_increase) ||
        !valid_fraction(floors.network_retransmit_minimum_fraction) ||
        !valid_fraction(floors.network_retransmit_minimum_increase) ||
        !valid_nonnegative(floors.network_quality_counter_minimum) ||
        !valid_nonnegative(floors.network_connectivity_minimum_severity)) {
        return std::unexpected{
            StatisticalAnalysisConfigurationError::resource_effect_floor_invalid};
    }
    if (!validate_workload_context_configuration(configuration.workload_context))
        return std::unexpected{
            StatisticalAnalysisConfigurationError::workload_context_invalid};
    return configuration;
}

StatisticalIncidentAnalyzer::StatisticalIncidentAnalyzer(
    const StatisticalAnalysisConfiguration configuration) {
    const auto validated = validate_statistical_analysis_configuration(configuration);
    if (!validated) throw std::invalid_argument{"Invalid statistical analysis configuration"};
    configuration_ = *validated;
}

std::expected<IncidentAnalysis, AnalysisError> StatisticalIncidentAnalyzer::analyze(
    const core::IncidentSnapshot& incident) const noexcept {
    try {
        if (incident.header().actual_end < incident.header().actual_start) {
            return std::unexpected{AnalysisError{
                AnalysisErrorCode::invalid_incident,
                "Incident end precedes its start"}};
        }
        return analyze_checked(incident);
    } catch (const std::bad_alloc&) {
        return std::unexpected{AnalysisError{
            AnalysisErrorCode::out_of_memory,
            "Insufficient memory for bounded incident analysis"}};
    } catch (...) {
        return std::unexpected{AnalysisError{
            AnalysisErrorCode::internal_error,
            "Unexpected statistical analysis failure"}};
    }
}

const StatisticalAnalysisConfiguration&
StatisticalIncidentAnalyzer::configuration() const noexcept {
    return configuration_;
}

IncidentAnalysis StatisticalIncidentAnalyzer::analyze_checked(
    const core::IncidentSnapshot& incident) const {
    IncidentAnalysis result{};
    const auto event = incident.header().window.event_time;
    result.baseline_end = event - configuration_.evaluation_pre_window;
    result.baseline_start = (std::max)(
        incident.header().actual_start,
        result.baseline_end - configuration_.baseline_duration);
    result.evaluation_start = (std::max)(
        incident.header().actual_start,
        event - configuration_.evaluation_pre_window);
    result.evaluation_end = incident.header().actual_end;

    MetricAccumulator cpu{MetricKind::system_cpu, configuration_.baseline_capacity,
                          configuration_.minimum_baseline_samples};
    MetricAccumulator memory{MetricKind::system_memory, configuration_.baseline_capacity,
                             configuration_.minimum_baseline_samples};
    MetricAccumulator disk_read{MetricKind::disk_read, configuration_.baseline_capacity,
                                configuration_.minimum_baseline_samples};
    MetricAccumulator disk_write{MetricKind::disk_write, configuration_.baseline_capacity,
                                 configuration_.minimum_baseline_samples};
    MetricAccumulator disk_read_latency{
        MetricKind::disk_read_latency, configuration_.baseline_capacity,
        configuration_.minimum_baseline_samples};
    MetricAccumulator disk_write_latency{
        MetricKind::disk_write_latency, configuration_.baseline_capacity,
        configuration_.minimum_baseline_samples};
    MetricAccumulator disk_service_time{
        MetricKind::disk_service_time, configuration_.baseline_capacity,
        configuration_.minimum_baseline_samples};
    MetricAccumulator disk_queue_depth{
        MetricKind::disk_queue_depth, configuration_.baseline_capacity,
        configuration_.minimum_baseline_samples};
    MetricAccumulator network_receive{
        MetricKind::network_receive, configuration_.baseline_capacity,
        configuration_.minimum_baseline_samples};
    MetricAccumulator network_transmit{
        MetricKind::network_transmit, configuration_.baseline_capacity,
        configuration_.minimum_baseline_samples};
    MetricAccumulator network_connectivity{
        MetricKind::network_connectivity, configuration_.baseline_capacity,
        configuration_.minimum_baseline_samples};
    MetricAccumulator network_interface_changes{
        MetricKind::network_interface_changes, configuration_.baseline_capacity,
        configuration_.minimum_baseline_samples};
    MetricAccumulator network_tcp_retransmit{
        MetricKind::network_tcp_retransmit, configuration_.baseline_capacity,
        configuration_.minimum_baseline_samples};
    MetricAccumulator network_tcp_failures{
        MetricKind::network_tcp_failures, configuration_.baseline_capacity,
        configuration_.minimum_baseline_samples};
    MetricAccumulator network_tcp_resets{
        MetricKind::network_tcp_resets, configuration_.baseline_capacity,
        configuration_.minimum_baseline_samples};

    for (const auto& sample : incident.system_samples()) {
        if (sample.observed_at >= result.baseline_start &&
            sample.observed_at < result.baseline_end) {
            ++result.system_samples_considered;
            cpu.add_baseline(sample.cpu_fraction);
            memory.add_baseline(sample.memory_fraction);
            disk_read.add_baseline(sample.disk_read_bytes_per_second);
            disk_write.add_baseline(sample.disk_write_bytes_per_second);
            disk_read_latency.add_baseline(sample.disk_read_latency_seconds);
            disk_write_latency.add_baseline(sample.disk_write_latency_seconds);
            disk_service_time.add_baseline(sample.disk_service_time_seconds);
            disk_queue_depth.add_baseline(sample.disk_queue_depth);
            network_receive.add_baseline(sample.network_receive_bytes_per_second);
            network_transmit.add_baseline(sample.network_transmit_bytes_per_second);
            network_connectivity.add_baseline(sample.network_connectivity_level);
            network_interface_changes.add_baseline(sample.network_interface_changes);
            network_tcp_retransmit.add_baseline(
                sample.network_tcp_retransmit_fraction);
            network_tcp_failures.add_baseline(
                sample.network_tcp_failed_connections);
            network_tcp_resets.add_baseline(sample.network_tcp_resets);
        }
    }
    cpu.prepare();
    memory.prepare();
    disk_read.prepare();
    disk_write.prepare();
    disk_read_latency.prepare();
    disk_write_latency.prepare();
    disk_service_time.prepare();
    disk_queue_depth.prepare();
    network_receive.prepare();
    network_transmit.prepare();
    network_connectivity.prepare();
    network_interface_changes.prepare();
    network_tcp_retransmit.prepare();
    network_tcp_failures.prepare();
    network_tcp_resets.prepare();
    for (const auto& sample : incident.system_samples()) {
        if (sample.observed_at < result.evaluation_start ||
            sample.observed_at > result.evaluation_end) continue;
        ++result.system_samples_considered;
        cpu.evaluate(sample.cpu_fraction, sample.observed_at);
        memory.evaluate(sample.memory_fraction, sample.observed_at);
        disk_read.evaluate(sample.disk_read_bytes_per_second, sample.observed_at);
        disk_write.evaluate(sample.disk_write_bytes_per_second, sample.observed_at);
        disk_read_latency.evaluate(sample.disk_read_latency_seconds,
                                   sample.observed_at);
        disk_write_latency.evaluate(sample.disk_write_latency_seconds,
                                    sample.observed_at);
        disk_service_time.evaluate(sample.disk_service_time_seconds,
                                   sample.observed_at);
        disk_queue_depth.evaluate(sample.disk_queue_depth, sample.observed_at);
        network_receive.evaluate(sample.network_receive_bytes_per_second, sample.observed_at);
        network_transmit.evaluate(sample.network_transmit_bytes_per_second, sample.observed_at);
        network_connectivity.evaluate(sample.network_connectivity_level,
                                      sample.observed_at);
        network_interface_changes.evaluate(sample.network_interface_changes,
                                           sample.observed_at);
        network_tcp_retransmit.evaluate(sample.network_tcp_retransmit_fraction,
                                        sample.observed_at);
        network_tcp_failures.evaluate(sample.network_tcp_failed_connections,
                                      sample.observed_at);
        network_tcp_resets.evaluate(sample.network_tcp_resets, sample.observed_at);
    }
    cpu.finish();
    memory.finish();
    disk_read.finish();
    disk_write.finish();
    disk_read_latency.finish();
    disk_write_latency.finish();
    disk_service_time.finish();
    disk_queue_depth.finish();
    network_receive.finish();
    network_transmit.finish();
    network_connectivity.finish();
    network_interface_changes.finish();
    network_tcp_retransmit.finish();
    network_tcp_failures.finish();
    network_tcp_resets.finish();
    const auto& floors = configuration_.resource_effect_floors;
    result.resources.push_back(resource(ResourceKind::cpu, {&cpu}, floors));
    result.resources.push_back(resource(ResourceKind::memory, {&memory}, floors));
    result.resources.push_back(resource(
        ResourceKind::disk,
        {&disk_read, &disk_write, &disk_read_latency, &disk_write_latency,
         &disk_service_time, &disk_queue_depth}, floors));
    result.resources.push_back(resource(ResourceKind::network,
                                        {&network_receive, &network_transmit,
                                         &network_connectivity,
                                         &network_interface_changes,
                                         &network_tcp_retransmit,
                                         &network_tcp_failures,
                                         &network_tcp_resets}, floors));
    std::sort(result.resources.begin(), result.resources.end(),
              [](const ResourceAnomaly& left, const ResourceAnomaly& right) {
                  if (left.score != right.score) return left.score > right.score;
                  return left.resource < right.resource;
              });

    const auto selected = select_process_candidates(
        incident, result.evaluation_start, result.evaluation_end,
        configuration_.maximum_process_candidates);
    std::vector<ProcessWork> work;
    work.reserve(selected.size());
    for (const auto identity : selected) work.emplace_back(identity, configuration_);
    for (const auto& sample : incident.process_samples()) {
        if (sample.observed_at < result.baseline_start ||
            sample.observed_at >= result.baseline_end) continue;
        auto* current = find_work(work, sample.identity);
        if (current == nullptr) continue;
        ++result.process_samples_considered;
        current->cpu.add_baseline(sample.cpu_fraction);
        current->memory.add_baseline(sample.working_set_bytes);
        current->read.add_baseline(sample.disk_read_bytes_per_second);
        current->write.add_baseline(sample.disk_write_bytes_per_second);
    }
    for (auto& current : work) {
        current.cpu.prepare();
        current.memory.prepare();
        current.read.prepare();
        current.write.prepare();
    }
    for (const auto& sample : incident.process_samples()) {
        if (sample.observed_at < result.evaluation_start ||
            sample.observed_at > result.evaluation_end) continue;
        auto* current = find_work(work, sample.identity);
        if (current == nullptr) continue;
        ++result.process_samples_considered;
        current->cpu.evaluate(sample.cpu_fraction, sample.observed_at);
        current->memory.evaluate(sample.working_set_bytes, sample.observed_at);
        current->read.evaluate(sample.disk_read_bytes_per_second, sample.observed_at);
        current->write.evaluate(sample.disk_write_bytes_per_second, sample.observed_at);
    }
    result.processes.reserve((std::min)(work.size(),
                                        configuration_.maximum_ranked_processes));
    for (auto& current : work) {
        current.cpu.finish();
        current.memory.finish();
        current.read.finish();
        current.write.finish();
        ProcessAnomaly anomaly{};
        anomaly.identity = current.identity;
        anomaly.name = process_name(incident, current.identity);
        anomaly.evidence = {current.cpu.evidence(), current.memory.evidence(),
                            current.read.evidence(), current.write.evidence()};
        anomaly.score = maximum_score(anomaly.evidence);
        anomaly.uncontextualized_score = anomaly.score;
        anomaly.confidence = confidence_for(anomaly.evidence);
        result.processes.push_back(std::move(anomaly));
    }
    result.workload_context = recognize_workload_context(
        incident, configuration_.workload_context);
    apply_workload_context(result, configuration_.workload_context);
    if (result.processes.size() > configuration_.maximum_ranked_processes)
        result.processes.resize(configuration_.maximum_ranked_processes);

    std::size_t available_resource_evidence{};
    const auto count_missing = [&](const MetricAnomalyEvidence& evidence) {
        result.missing_values += evidence.missing_baseline_samples +
                                 evidence.missing_evaluation_samples;
        if (evidence.availability == EvidenceAvailability::available)
            ++available_resource_evidence;
    };
    for (const auto& ranked : result.resources)
        for (const auto& evidence : ranked.evidence) count_missing(evidence);
    for (const auto& ranked : result.processes)
        for (const auto& evidence : ranked.evidence)
            result.missing_values += evidence.missing_baseline_samples +
                                     evidence.missing_evaluation_samples;
    result.cold_start = available_resource_evidence == 0U;
    result.contributors = rank_contributors(incident, result);
    return result;
}

} // namespace blackbox::analysis
