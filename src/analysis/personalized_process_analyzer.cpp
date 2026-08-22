#include "analysis/personalized_process_analyzer.hpp"

#include "analysis/contributor_ranker.hpp"
#include "analysis/robust_baseline.hpp"
#include "analysis/workload_context.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>

namespace blackbox::analysis {
namespace {

[[nodiscard]] bool ascii_space(const unsigned char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

[[nodiscard]] std::string trim(std::string_view value) {
    while (!value.empty() && ascii_space(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && ascii_space(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1U);
    }
    return std::string{value};
}

[[nodiscard]] std::string normalized_component(std::string_view value,
                                               const bool path) {
    auto result = trim(value);
    if (path && result.starts_with("\\\\?\\")) result.erase(0U, 4U);
    std::string normalized;
    normalized.reserve(result.size());
    bool previous_separator = false;
    for (auto character : result) {
        if (path && character == '/') character = '\\';
        const auto separator = path && character == '\\';
        if (separator && previous_separator) continue;
        previous_separator = separator;
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
        normalized.push_back(character);
    }
    while (path && normalized.size() > 3U && normalized.back() == '\\') {
        normalized.pop_back();
    }
    return normalized;
}

[[nodiscard]] const core::IncidentProcessInfo* metadata_for(
    const core::IncidentSnapshot& incident,
    const core::IncidentProcessIdentity identity) noexcept {
    for (const auto& process : incident.process_metadata()) {
        if (process.identity == identity) return &process;
    }
    return nullptr;
}

[[nodiscard]] std::optional<double> observed_metric(
    const ProcessAnomaly& process, const MetricKind metric) noexcept {
    for (const auto& evidence : process.evidence) {
        if (evidence.metric == metric &&
            evidence.availability == EvidenceAvailability::available &&
            std::isfinite(evidence.observed_value)) {
            return evidence.observed_value;
        }
    }
    return std::nullopt;
}

void retain_maximum(std::optional<double>& destination,
                    const std::optional<double> value) noexcept {
    if (value && (!destination || *value > *destination)) destination = value;
}

[[nodiscard]] std::optional<double> history_metric(
    const ExecutableProfileObservation& observation,
    const MetricKind metric) noexcept {
    switch (metric) {
    case MetricKind::process_cpu: return observation.cpu_fraction;
    case MetricKind::process_working_set: return observation.working_set_bytes;
    case MetricKind::process_disk_read: return observation.disk_read_bytes_per_second;
    case MetricKind::process_disk_write: return observation.disk_write_bytes_per_second;
    default: return std::nullopt;
    }
}

[[nodiscard]] MetricAnomalyEvidence personalized_evidence(
    const MetricKind metric, const double observed,
    const std::string_view key, const IncidentAnalysisContext& context,
    const PersonalizedAnalysisConfiguration& configuration) {
    RollingRobustBaseline baseline{configuration.maximum_profile_observations};
    std::size_t missing{};
    const auto oldest = context.observed_utc_milliseconds -
                        configuration.maximum_profile_age.count();
    for (const auto& observation : context.executable_history) {
        if (observation.executable_key != key ||
            observation.observed_utc_milliseconds > context.observed_utc_milliseconds ||
            (observation.observed_utc_milliseconds == context.observed_utc_milliseconds &&
             observation.incident_id >= context.incident_id) ||
            observation.observed_utc_milliseconds < oldest) {
            continue;
        }
        const auto value = history_metric(observation, metric);
        if (value && std::isfinite(*value)) baseline.add(*value);
        else ++missing;
    }
    MetricAnomalyEvidence evidence{};
    evidence.metric = metric;
    evidence.baseline_scope = BaselineScope::personalized_executable;
    evidence.observed_value = observed;
    evidence.evaluation_samples = 1U;
    evidence.missing_baseline_samples = missing;
    evidence.baseline = baseline.summarize();
    if (baseline.size() < configuration.minimum_profile_observations) {
        evidence.availability = EvidenceAvailability::insufficient_baseline;
        return evidence;
    }
    evidence.availability = EvidenceAvailability::available;
    evidence.robust_z = robust_z_score(evidence.baseline, observed);
    evidence.baseline_percentile = baseline_percentile(baseline, observed);
    evidence.score = anomaly_score(std::abs(evidence.robust_z));
    evidence.direction = evidence.robust_z > 0.0 ? AnomalyDirection::higher
                         : evidence.robust_z < 0.0 ? AnomalyDirection::lower
                                                  : AnomalyDirection::unchanged;
    return evidence;
}

[[nodiscard]] AnalysisConfidence profile_confidence(const std::size_t count) noexcept {
    if (count >= 30U) return AnalysisConfidence::high;
    if (count >= 8U) return AnalysisConfidence::moderate;
    return AnalysisConfidence::low;
}

[[nodiscard]] double maximum_score(
    const std::vector<MetricAnomalyEvidence>& evidence) noexcept {
    double result{};
    for (const auto& item : evidence) result = (std::max)(result, item.score);
    return result;
}

} // namespace

std::optional<NormalizedExecutableIdentity> normalize_executable_identity(
    const core::IncidentProcessInfo& process) {
    const auto name_available = process.name.status == core::RecordedValueStatus::available;
    const auto path_available =
        process.executable_path.status == core::RecordedValueStatus::available;
    const auto display_name = name_available ? trim(process.name.value) : std::string{};
    if (path_available) {
        const auto path = normalized_component(process.executable_path.value, true);
        if (!path.empty() && path.size() + 5U <= maximum_executable_identity_key_bytes) {
            return NormalizedExecutableIdentity{"path:" + path,
                                                display_name.empty() ? path : display_name,
                                                ExecutableIdentitySource::normalized_path};
        }
    }
    if (name_available) {
        const auto name = normalized_component(process.name.value, false);
        if (!name.empty() && name.size() + 5U <= maximum_executable_identity_key_bytes) {
            return NormalizedExecutableIdentity{"name:" + name, display_name,
                                                ExecutableIdentitySource::normalized_name};
        }
    }
    return std::nullopt;
}

std::expected<PersonalizedAnalysisConfiguration,
              PersonalizedAnalysisConfigurationError>
validate_personalized_analysis_configuration(
    const PersonalizedAnalysisConfiguration configuration) noexcept {
    if (!validate_statistical_analysis_configuration(configuration.incident_local)) {
        return std::unexpected{
            PersonalizedAnalysisConfigurationError::invalid_incident_local_configuration};
    }
    if (configuration.maximum_profile_age <= std::chrono::milliseconds::zero()) {
        return std::unexpected{
            PersonalizedAnalysisConfigurationError::profile_age_not_positive};
    }
    if (configuration.minimum_profile_observations == 0U ||
        configuration.minimum_profile_observations >
            configuration.maximum_profile_observations) {
        return std::unexpected{
            PersonalizedAnalysisConfigurationError::minimum_profile_observations_invalid};
    }
    if (configuration.maximum_profile_updates == 0U ||
        configuration.maximum_profile_updates >
            configuration.incident_local.maximum_process_candidates) {
        return std::unexpected{
            PersonalizedAnalysisConfigurationError::maximum_profile_updates_invalid};
    }
    return configuration;
}

PersonalizedProcessAnalyzer::PersonalizedProcessAnalyzer(
    const PersonalizedAnalysisConfiguration configuration)
    : configuration_{configuration}, incident_local_{configuration.incident_local} {
    if (!validate_personalized_analysis_configuration(configuration)) {
        throw std::invalid_argument{"invalid personalized analysis configuration"};
    }
}

std::expected<IncidentAnalysis, AnalysisError> PersonalizedProcessAnalyzer::analyze(
    const core::IncidentSnapshot& incident) const noexcept {
    return analyze(incident, IncidentAnalysisContext{});
}

std::expected<IncidentAnalysis, AnalysisError> PersonalizedProcessAnalyzer::analyze(
    const core::IncidentSnapshot& incident,
    const IncidentAnalysisContext& context) const noexcept {
    auto local = incident_local_.analyze(incident);
    if (!local) return local;
    try {
        return personalize(incident, context, std::move(*local));
    } catch (const std::bad_alloc&) {
        return std::unexpected{AnalysisError{AnalysisErrorCode::out_of_memory,
                                             "personalized analysis allocation failed"}};
    } catch (const std::exception& exception) {
        return std::unexpected{AnalysisError{AnalysisErrorCode::internal_error,
                                             exception.what()}};
    } catch (...) {
        return std::unexpected{AnalysisError{AnalysisErrorCode::internal_error,
                                             "unknown personalized analysis failure"}};
    }
}

bool PersonalizedProcessAnalyzer::uses_personalized_history() const noexcept {
    return true;
}

const PersonalizedAnalysisConfiguration&
PersonalizedProcessAnalyzer::configuration() const noexcept {
    return configuration_;
}

IncidentAnalysis PersonalizedProcessAnalyzer::personalize(
    const core::IncidentSnapshot& incident,
    const IncidentAnalysisContext& context,
    IncidentAnalysis analysis) const {
    std::map<std::string, ExecutableProfileObservation, std::less<>> updates;
    for (auto& process : analysis.processes) {
        const auto* metadata = metadata_for(incident, process.identity);
        if (metadata == nullptr) continue;
        const auto executable = normalize_executable_identity(*metadata);
        if (!executable) continue;
        process.executable_key = executable->key;
        process.personalization = PersonalizationState::cold_start;

        auto [update, inserted] = updates.try_emplace(executable->key);
        if (inserted) {
            update->second.executable_key = executable->key;
            update->second.display_name = executable->display_name;
            update->second.incident_id = context.incident_id;
            update->second.observed_utc_milliseconds = context.observed_utc_milliseconds;
        }
        retain_maximum(update->second.cpu_fraction,
                       observed_metric(process, MetricKind::process_cpu));
        retain_maximum(update->second.working_set_bytes,
                       observed_metric(process, MetricKind::process_working_set));
        retain_maximum(update->second.disk_read_bytes_per_second,
                       observed_metric(process, MetricKind::process_disk_read));
        retain_maximum(update->second.disk_write_bytes_per_second,
                       observed_metric(process, MetricKind::process_disk_write));

        std::size_t personalized_count{};
        bool personalized_any = false;
        for (auto& evidence : process.evidence) {
            if (evidence.availability != EvidenceAvailability::available) continue;
            auto replacement = personalized_evidence(
                evidence.metric, evidence.observed_value, executable->key,
                context, configuration_);
            personalized_count = (std::max)(personalized_count,
                                             replacement.baseline.sample_count);
            if (replacement.availability == EvidenceAvailability::available) {
                evidence = std::move(replacement);
                personalized_any = true;
            }
        }
        process.personalized_observations = personalized_count;
        if (personalized_any) {
            process.personalization = PersonalizationState::ready;
            process.uncontextualized_score = maximum_score(process.evidence);
            process.score = process.uncontextualized_score;
            process.confidence = profile_confidence(personalized_count);
        } else {
            process.confidence = AnalysisConfidence::low;
        }
    }
    analysis.profile_updates.reserve((std::min)(updates.size(),
                                                 configuration_.maximum_profile_updates));
    for (auto& [key, update] : updates) {
        static_cast<void>(key);
        if (analysis.profile_updates.size() == configuration_.maximum_profile_updates) break;
        analysis.profile_updates.push_back(std::move(update));
    }
    apply_workload_context(analysis, configuration_.incident_local.workload_context);
    analysis.contributors = rank_contributors(incident, analysis, context);
    return analysis;
}

} // namespace blackbox::analysis
