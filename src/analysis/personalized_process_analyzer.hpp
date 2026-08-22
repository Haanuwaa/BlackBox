#pragma once

#include "analysis/incident_analyzer.hpp"
#include "analysis/statistical_incident_analyzer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace blackbox::analysis {

inline constexpr std::size_t maximum_executable_identity_key_bytes = 2'048U;

struct PersonalizedAnalysisConfiguration {
    StatisticalAnalysisConfiguration incident_local{};
    std::chrono::milliseconds maximum_profile_age{std::chrono::hours{24 * 30}};
    std::size_t minimum_profile_observations{8U};
    std::size_t maximum_profile_observations{64U};
    std::size_t maximum_profile_updates{512U};
    friend constexpr bool operator==(const PersonalizedAnalysisConfiguration&,
                                     const PersonalizedAnalysisConfiguration&) = default;
};

enum class PersonalizedAnalysisConfigurationError : std::uint8_t {
    invalid_incident_local_configuration,
    profile_age_not_positive,
    minimum_profile_observations_invalid,
    maximum_profile_updates_invalid,
};

[[nodiscard]] std::expected<PersonalizedAnalysisConfiguration,
                            PersonalizedAnalysisConfigurationError>
validate_personalized_analysis_configuration(
    PersonalizedAnalysisConfiguration configuration) noexcept;

class PersonalizedProcessAnalyzer final : public IIncidentAnalyzer {
public:
    explicit PersonalizedProcessAnalyzer(
        PersonalizedAnalysisConfiguration configuration = {});

    [[nodiscard]] std::expected<IncidentAnalysis, AnalysisError> analyze(
        const core::IncidentSnapshot& incident) const noexcept override;
    [[nodiscard]] std::expected<IncidentAnalysis, AnalysisError> analyze(
        const core::IncidentSnapshot& incident,
        const IncidentAnalysisContext& context) const noexcept override;
    [[nodiscard]] bool uses_personalized_history() const noexcept override;
    [[nodiscard]] const PersonalizedAnalysisConfiguration& configuration() const noexcept;

private:
    [[nodiscard]] IncidentAnalysis personalize(
        const core::IncidentSnapshot& incident,
        const IncidentAnalysisContext& context,
        IncidentAnalysis analysis) const;

    PersonalizedAnalysisConfiguration configuration_{};
    StatisticalIncidentAnalyzer incident_local_{};
};

} // namespace blackbox::analysis
