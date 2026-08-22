#pragma once

#include "analysis/personalized_process_analyzer.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>

namespace blackbox::analysis {

inline constexpr std::uint32_t intelligent_pipeline_version = 13U;
inline constexpr std::uint32_t diagnosis_evidence_model_version = 12U;
inline constexpr std::size_t maximum_diagnosis_evidence_links = 8U;

struct IntelligentAnalysisConfiguration {
    PersonalizedAnalysisConfiguration component_analysis{};
    FeedbackCalibrationConfiguration feedback_calibration{};
    ContributorFeedbackCalibrationConfiguration contributor_feedback_calibration{};
    SimilarIncidentEvidenceConfiguration similar_incident_evidence{};
    double minimum_diagnosis_resource_score{0.35};
    double minimum_feedback_adjusted_assertion_confidence{0.35};
    double multi_resource_minimum_score{0.65};
    double multi_resource_maximum_gap{0.08};
    std::size_t maximum_evidence_links{maximum_diagnosis_evidence_links};
    friend constexpr bool operator==(const IntelligentAnalysisConfiguration&,
                                     const IntelligentAnalysisConfiguration&) = default;
};

enum class IntelligentAnalysisConfigurationError : std::uint8_t {
    component_configuration_invalid,
    feedback_configuration_invalid,
    contributor_feedback_configuration_invalid,
    similar_incident_configuration_invalid,
    diagnosis_threshold_invalid,
    feedback_assertion_threshold_invalid,
    multi_resource_threshold_invalid,
    multi_resource_gap_invalid,
    evidence_limit_invalid,
};

[[nodiscard]] std::expected<IntelligentAnalysisConfiguration,
                            IntelligentAnalysisConfigurationError>
validate_intelligent_analysis_configuration(
    IntelligentAnalysisConfiguration configuration) noexcept;

[[nodiscard]] std::uint64_t intelligent_configuration_fingerprint(
    const IntelligentAnalysisConfiguration& configuration) noexcept;

[[nodiscard]] IncidentDiagnosis compose_incident_diagnosis(
    const core::IncidentSnapshot& incident,
    const IncidentAnalysis& analysis,
    const IncidentAnalysisContext& context,
    const IntelligentAnalysisConfiguration& configuration = {});

[[nodiscard]] bool diagnosis_evidence_links_valid(
    const core::IncidentSnapshot& incident,
    const IncidentAnalysis& analysis) noexcept;

class IntelligentIncidentAnalyzer final : public IIncidentAnalyzer {
public:
    explicit IntelligentIncidentAnalyzer(
        IntelligentAnalysisConfiguration configuration = {});

    [[nodiscard]] std::expected<IncidentAnalysis, AnalysisError> analyze(
        const core::IncidentSnapshot& incident) const noexcept override;
    [[nodiscard]] std::expected<IncidentAnalysis, AnalysisError> analyze(
        const core::IncidentSnapshot& incident,
        const IncidentAnalysisContext& context) const noexcept override;
    [[nodiscard]] bool uses_personalized_history() const noexcept override;
    [[nodiscard]] std::uint32_t pipeline_version() const noexcept override;
    [[nodiscard]] const IntelligentAnalysisConfiguration& configuration() const noexcept;

private:
    IntelligentAnalysisConfiguration configuration_{};
    PersonalizedProcessAnalyzer components_{};
};

} // namespace blackbox::analysis
