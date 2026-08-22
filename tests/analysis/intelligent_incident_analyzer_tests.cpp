#include "diagnosis_fixture.hpp"
#include "analysis/intelligent_incident_analyzer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace analysis = blackbox::analysis;
namespace fixture = blackbox::test::diagnosis_fixture;

TEST_CASE("intelligent analysis configuration and fingerprint are stable and explicit",
          "[analysis][pipeline][configuration][version]") {
    CHECK(analysis::validate_intelligent_analysis_configuration({}).has_value());
    auto invalid = analysis::IntelligentAnalysisConfiguration{};
    invalid.maximum_evidence_links = analysis::maximum_diagnosis_evidence_links + 1U;
    CHECK_FALSE(analysis::validate_intelligent_analysis_configuration(invalid).has_value());
    invalid = {};
    invalid.minimum_diagnosis_resource_score = 0.0;
    CHECK_FALSE(analysis::validate_intelligent_analysis_configuration(invalid).has_value());
    invalid = {};
    invalid.feedback_calibration.minimum_matching_observations = 0U;
    CHECK_FALSE(analysis::validate_intelligent_analysis_configuration(invalid).has_value());
    invalid = {};
    invalid.minimum_feedback_adjusted_assertion_confidence = 0.0;
    CHECK_FALSE(analysis::validate_intelligent_analysis_configuration(invalid).has_value());

    const auto first = analysis::intelligent_configuration_fingerprint({});
    const auto second = analysis::intelligent_configuration_fingerprint({});
    CHECK(first == second);
    auto changed = analysis::IntelligentAnalysisConfiguration{};
    changed.minimum_diagnosis_resource_score = 0.40;
    CHECK(analysis::intelligent_configuration_fingerprint(changed) != first);
    changed = {};
    changed.feedback_calibration.minimum_matching_observations = 5U;
    CHECK(analysis::intelligent_configuration_fingerprint(changed) != first);
    changed = {};
    changed.contributor_feedback_calibration.minimum_matching_observations = 5U;
    CHECK(analysis::intelligent_configuration_fingerprint(changed) != first);
    changed = {};
    changed.similar_incident_evidence.minimum_matching_confirmations = 3U;
    CHECK(analysis::intelligent_configuration_fingerprint(changed) != first);
}

TEST_CASE("explicit attribution history reranks only matching future contributors",
          "[analysis][pipeline][contributor-feedback][ranking]") {
    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto input = fixture::incident(analysis::ResourceKind::cpu,
                                         std::nullopt, true);
    const auto baseline = analyzer.analyze(*input);
    REQUIRE(baseline.has_value());
    REQUIRE_FALSE(baseline->contributors.empty());
    const auto& original = baseline->contributors.front();
    REQUIRE_FALSE(original.executable_key.empty());
    std::vector<analysis::ContributorFeedbackObservation> history;
    for (std::int64_t id = 1; id <= 4; ++id) {
        history.push_back({
            id, id * 1'000, id * 1'000 + 1, original.executable_key,
            original.matched_resource,
            analysis::ContributorFeedbackDisposition::not_a_contributor});
    }
    const analysis::IncidentAnalysisContext context{
        .incident_id = 20,
        .observed_utc_milliseconds = 20'000,
        .contributor_feedback_history = history};
    const auto calibrated = analyzer.analyze(*input, context);
    REQUIRE(calibrated.has_value());
    REQUIRE_FALSE(calibrated->contributors.empty());
    const auto& adjusted = calibrated->contributors.front();
    CHECK(adjusted.executable_key == original.executable_key);
    CHECK(adjusted.feedback_state ==
          analysis::ContributorFeedbackState::reduced);
    CHECK(adjusted.score_before_feedback == original.score);
    CHECK(adjusted.score < adjusted.score_before_feedback);
    CHECK(calibrated->resources == baseline->resources);
    CHECK(calibrated->processes == baseline->processes);
    CHECK(analysis::diagnosis_evidence_links_valid(*input, *calibrated));
}

TEST_CASE("repeated matching false positives reduce only future automatic assertions",
          "[analysis][pipeline][feedback][false-positive]") {
    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto input = fixture::incident(analysis::ResourceKind::cpu,
                                         std::nullopt, true);
    const auto uncalibrated = analyzer.analyze(*input);
    REQUIRE(uncalibrated.has_value());
    REQUIRE(uncalibrated->diagnosis.available);

    std::vector<analysis::FeedbackObservation> history;
    for (std::int64_t id = 1; id <= 8; ++id) {
        history.push_back({
            id, id * 1'000,
            blackbox::core::AutomaticIncidentResource::cpu,
            blackbox::core::AutomaticIncidentSignal::throughput_or_utilization,
            analysis::FeedbackDisposition::false_positive});
    }
    const analysis::IncidentAnalysisContext context{
        20, 20'000, {}, {}, history};
    const auto calibrated = analyzer.analyze(*input, context);
    REQUIRE(calibrated.has_value());
    CHECK(calibrated->feedback_calibration.state ==
          analysis::FeedbackCalibrationState::suppressing);
    CHECK(calibrated->diagnosis.confidence_before_feedback ==
          uncalibrated->diagnosis.calibrated_confidence);
    CHECK(calibrated->diagnosis.feedback_multiplier < 1.0);
    CHECK(calibrated->diagnosis.calibrated_confidence <
          uncalibrated->diagnosis.calibrated_confidence);
    CHECK(calibrated->resources == uncalibrated->resources);
    CHECK(calibrated->processes == uncalibrated->processes);
    CHECK(calibrated->contributors == uncalibrated->contributors);
    CHECK(analysis::diagnosis_evidence_links_valid(*input, *calibrated));
}

TEST_CASE("feedback calibration abstains instead of publishing a weakened assertion",
          "[analysis][pipeline][feedback][abstention]") {
    auto configuration = analysis::IntelligentAnalysisConfiguration{};
    configuration.minimum_feedback_adjusted_assertion_confidence = 0.99;
    analysis::IntelligentIncidentAnalyzer analyzer{configuration};
    const auto input = fixture::incident(analysis::ResourceKind::cpu,
                                         std::nullopt, true);
    std::vector<analysis::FeedbackObservation> history;
    for (std::int64_t id = 1; id <= 4; ++id) {
        history.push_back({
            id, id * 1'000,
            blackbox::core::AutomaticIncidentResource::cpu,
            blackbox::core::AutomaticIncidentSignal::throughput_or_utilization,
            analysis::FeedbackDisposition::false_positive});
    }
    const auto result = analyzer.analyze(
        *input, analysis::IncidentAnalysisContext{20, 20'000, {}, {}, history});
    REQUIRE(result.has_value());
    CHECK(result->diagnosis.suppressed_by_feedback);
    CHECK_FALSE(result->diagnosis.available);
    CHECK(result->diagnosis.type == analysis::IncidentType::unknown);
    CHECK(result->diagnosis.basis == analysis::SymptomExplanationBasis::none);
    CHECK(result->diagnosis.evidence.empty() == false);
    CHECK(analysis::diagnosis_evidence_links_valid(*input, *result));
}

TEST_CASE("versioned pipeline diagnoses labeled resource incidents reproducibly",
          "[analysis][pipeline][quality][determinism]") {
    analysis::IntelligentIncidentAnalyzer analyzer;
    CHECK(analyzer.pipeline_version() == analysis::intelligent_pipeline_version);
    for (const auto resource : {analysis::ResourceKind::cpu,
                                analysis::ResourceKind::memory,
                                analysis::ResourceKind::disk,
                                analysis::ResourceKind::network}) {
        const auto input = fixture::incident(resource, std::nullopt, true);
        const auto first = analyzer.analyze(*input);
        const auto second = analyzer.analyze(*input);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(*first == *second);
        CHECK(first->provenance.pipeline_version ==
              analysis::intelligent_pipeline_version);
        CHECK(first->provenance.evidence_model_version ==
              analysis::diagnosis_evidence_model_version);
        CHECK(first->provenance.configuration_fingerprint != 0U);
        CHECK(first->provenance.native_inference ==
              analysis::NativeInferenceStatus::not_adopted);
        CHECK(first->diagnosis.available);
        CHECK(first->diagnosis.type == fixture::expected_type(resource));
        CHECK(first->diagnosis.basis ==
              analysis::SymptomExplanationBasis::automatic_capture_alignment);
        CHECK(first->diagnosis.confidence != analysis::AnalysisConfidence::unavailable);
        CHECK(first->diagnosis.evidence.size() <=
              analysis::maximum_diagnosis_evidence_links);
        CHECK(analysis::diagnosis_evidence_links_valid(*input, *first));
        CHECK(std::any_of(first->diagnosis.evidence.begin(),
                          first->diagnosis.evidence.end(), [](const auto& link) {
            return link.kind == analysis::DiagnosisEvidenceKind::resource_anomaly;
        }));
    }
}

TEST_CASE("quiet incidents remain undiagnosed instead of forcing a conclusion",
          "[analysis][pipeline][unknown][false-positive]") {
    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto input = fixture::incident(std::nullopt);
    const auto result = analyzer.analyze(*input);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->diagnosis.available);
    CHECK(result->diagnosis.type == analysis::IncidentType::unknown);
    CHECK(result->diagnosis.confidence == analysis::AnalysisConfidence::unavailable);
    CHECK(result->diagnosis.evidence.empty());
    CHECK(analysis::diagnosis_evidence_links_valid(*input, *result));
}

TEST_CASE("automatic trigger evidence remains inspectable without broadening its resource",
          "[analysis][pipeline][multi-resource][trigger]") {
    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto input = fixture::incident(
        analysis::ResourceKind::cpu, analysis::ResourceKind::memory, true);
    const auto result = analyzer.analyze(*input);
    REQUIRE(result.has_value());
    CHECK(result->diagnosis.type == analysis::IncidentType::cpu_pressure);
    CHECK(result->diagnosis.basis ==
          analysis::SymptomExplanationBasis::automatic_capture_alignment);
    CHECK(std::any_of(result->diagnosis.evidence.begin(),
                      result->diagnosis.evidence.end(), [](const auto& link) {
        return link.kind == analysis::DiagnosisEvidenceKind::automatic_capture_trigger;
    }));
    CHECK(analysis::diagnosis_evidence_links_valid(*input, *result));

    const auto manual = fixture::incident(
        analysis::ResourceKind::cpu, analysis::ResourceKind::memory, false);
    const auto manual_result = analyzer.analyze(*manual);
    REQUIRE(manual_result.has_value());
    CHECK_FALSE(manual_result->diagnosis.available);
    CHECK(manual_result->diagnosis.type == analysis::IncidentType::unknown);
    CHECK(std::any_of(manual_result->resources.begin(), manual_result->resources.end(),
                      [](const auto& resource) { return resource.score > 0.0; }));
}

TEST_CASE("automatic trigger resource disambiguates a nearby stronger incidental score",
          "[analysis][pipeline][trigger][disambiguation]") {
    const auto input = fixture::incident(analysis::ResourceKind::cpu,
                                         std::nullopt, true);
    analysis::PersonalizedProcessAnalyzer components;
    const auto component_result = components.analyze(*input);
    REQUIRE(component_result.has_value());
    auto analysis = *component_result;
    for (auto& resource : analysis.resources) {
        if (resource.resource == analysis::ResourceKind::cpu) resource.score = 0.90;
        if (resource.resource == analysis::ResourceKind::network) resource.score = 0.95;
    }

    const auto diagnosis = analysis::compose_incident_diagnosis(
        *input, analysis, {});
    REQUIRE(diagnosis.available);
    CHECK(diagnosis.type == analysis::IncidentType::cpu_pressure);
    const auto resource_link = std::find_if(
        diagnosis.evidence.begin(), diagnosis.evidence.end(), [](const auto& link) {
            return link.kind == analysis::DiagnosisEvidenceKind::resource_anomaly;
        });
    REQUIRE(resource_link != diagnosis.evidence.end());
    CHECK(analysis.resources[resource_link->source_index].resource ==
          analysis::ResourceKind::cpu);
    CHECK(std::any_of(diagnosis.evidence.begin(), diagnosis.evidence.end(),
                      [](const auto& link) {
        return link.kind == analysis::DiagnosisEvidenceKind::automatic_capture_trigger;
    }));

    auto manual_header = input->header();
    manual_header.window.automatic_trigger_count = 0U;
    manual_header.window.automatic_resource =
        blackbox::core::AutomaticIncidentResource::none;
    const blackbox::core::IncidentSnapshot manual{
        manual_header,
        {input->system_samples().begin(), input->system_samples().end()},
        {input->process_metadata().begin(), input->process_metadata().end()},
        {input->process_samples().begin(), input->process_samples().end()},
        {input->system_events().begin(), input->system_events().end()}};
    CHECK_FALSE(analysis::compose_incident_diagnosis(manual, analysis, {}).available);
}

TEST_CASE("practical resource pressure remains an observation without symptom alignment",
          "[analysis][pipeline][unknown][pressure-separation]") {
    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto input = fixture::incident(analysis::ResourceKind::network);
    const auto result = analyzer.analyze(*input);
    REQUIRE(result.has_value());
    const auto pressure = std::find_if(
        result->resources.begin(), result->resources.end(), [](const auto& resource) {
            return resource.resource == analysis::ResourceKind::network;
        });
    REQUIRE(pressure != result->resources.end());
    CHECK(pressure->score > 0.99);
    CHECK(pressure->pressure_metric == analysis::MetricKind::network_receive);
    CHECK_FALSE(result->diagnosis.available);
    CHECK(result->diagnosis.basis == analysis::SymptomExplanationBasis::none);
    CHECK(result->diagnosis.evidence.empty());
}

TEST_CASE("recorded application hang is a direct symptom explanation without resource pressure",
          "[analysis][pipeline][hang][system-event]") {
    const auto base = fixture::incident(std::nullopt);
    auto header = base->header();
    header.window.automatic_trigger_count = 1U;
    header.window.manual_trigger_count = 0U;
    header.window.automatic_score = 1.0;
    header.window.automatic_signal =
        blackbox::core::AutomaticIncidentSignal::application_hang;
    const blackbox::core::SystemEvent hang{
        .observed_at = header.window.event_time,
        .source = blackbox::core::SystemEventSource::application,
        .kind = blackbox::core::SystemEventKind::application_hang,
        .level = blackbox::core::SystemEventLevel::warning,
        .native_event_id = 1002U};
    const blackbox::core::IncidentSnapshot input{
        header,
        {base->system_samples().begin(), base->system_samples().end()},
        {base->process_metadata().begin(), base->process_metadata().end()},
        {base->process_samples().begin(), base->process_samples().end()},
        {hang}};

    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(input);
    REQUIRE(result.has_value());
    CHECK(result->diagnosis.available);
    CHECK(result->diagnosis.type == analysis::IncidentType::application_hang);
    CHECK(result->diagnosis.basis ==
          analysis::SymptomExplanationBasis::automatic_capture_alignment);
    CHECK(result->diagnosis.confidence == analysis::AnalysisConfidence::high);
    CHECK(analysis::diagnosis_evidence_links_valid(input, *result));
    CHECK(std::any_of(result->diagnosis.evidence.begin(),
                      result->diagnosis.evidence.end(), [](const auto& link) {
        return link.kind == analysis::DiagnosisEvidenceKind::system_event;
    }));
}

TEST_CASE("recorded application crash is an exact Windows symptom not a root-cause claim",
          "[analysis][pipeline][crash][system-event]") {
    const auto base = fixture::incident(std::nullopt);
    auto header = base->header();
    header.window.automatic_trigger_count = 1U;
    header.window.manual_trigger_count = 0U;
    header.window.automatic_score = 1.0;
    header.window.automatic_signal =
        blackbox::core::AutomaticIncidentSignal::application_crash;
    const blackbox::core::SystemEvent crash{
        .observed_at = header.window.event_time,
        .source = blackbox::core::SystemEventSource::application,
        .kind = blackbox::core::SystemEventKind::application_crash,
        .level = blackbox::core::SystemEventLevel::error,
        .native_event_id = 1000U};
    const blackbox::core::IncidentSnapshot input{
        header,
        {base->system_samples().begin(), base->system_samples().end()},
        {base->process_metadata().begin(), base->process_metadata().end()},
        {base->process_samples().begin(), base->process_samples().end()},
        {crash}};

    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(input);
    REQUIRE(result.has_value());
    CHECK(result->diagnosis.available);
    CHECK(result->diagnosis.type == analysis::IncidentType::application_crash);
    CHECK(result->diagnosis.basis ==
          analysis::SymptomExplanationBasis::automatic_capture_alignment);
    CHECK(result->diagnosis.confidence == analysis::AnalysisConfidence::high);
    CHECK(analysis::diagnosis_evidence_links_valid(input, *result));

    header.window.automatic_trigger_count = 0U;
    header.window.manual_trigger_count = 1U;
    auto wrong_event = crash;
    wrong_event.native_event_id = 1002U;
    const blackbox::core::IncidentSnapshot mismatched{
        header,
        {base->system_samples().begin(), base->system_samples().end()},
        {base->process_metadata().begin(), base->process_metadata().end()},
        {base->process_samples().begin(), base->process_samples().end()},
        {wrong_event}};
    const auto mismatch = analyzer.analyze(mismatched);
    REQUIRE(mismatch.has_value());
    CHECK_FALSE(mismatch->diagnosis.available);
}

TEST_CASE("display timeout recovery is a precise automatic Windows symptom",
          "[analysis][pipeline][graphics][system-event]") {
    const auto base = fixture::incident(std::nullopt);
    auto header = base->header();
    header.window.automatic_trigger_count = 1U;
    header.window.manual_trigger_count = 0U;
    header.window.automatic_resource = blackbox::core::AutomaticIncidentResource::none;
    header.window.automatic_score = 1.0;
    header.window.automatic_signal =
        blackbox::core::AutomaticIncidentSignal::display_driver_recovery;
    const blackbox::core::SystemEvent recovery{
        .observed_at = header.window.event_time,
        .source = blackbox::core::SystemEventSource::graphics,
        .kind = blackbox::core::SystemEventKind::display_driver_recovery,
        .level = blackbox::core::SystemEventLevel::warning,
        .native_event_id = 4101U};
    const blackbox::core::IncidentSnapshot input{
        header,
        {base->system_samples().begin(), base->system_samples().end()},
        {base->process_metadata().begin(), base->process_metadata().end()},
        {base->process_samples().begin(), base->process_samples().end()},
        {recovery}};

    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(input);
    REQUIRE(result.has_value());
    CHECK(result->diagnosis.available);
    CHECK(result->diagnosis.type == analysis::IncidentType::display_driver_recovery);
    CHECK(result->diagnosis.basis ==
          analysis::SymptomExplanationBasis::automatic_capture_alignment);
    CHECK(result->diagnosis.calibrated_confidence == 0.99);
    CHECK(std::any_of(result->diagnosis.evidence.begin(),
                      result->diagnosis.evidence.end(), [](const auto& link) {
        return link.kind == analysis::DiagnosisEvidenceKind::system_event;
    }));
    CHECK(std::any_of(result->diagnosis.evidence.begin(),
                      result->diagnosis.evidence.end(), [](const auto& link) {
        return link.kind == analysis::DiagnosisEvidenceKind::automatic_capture_trigger;
    }));
    CHECK(analysis::diagnosis_evidence_links_valid(input, *result));
}

TEST_CASE("mismatched display recovery identity cannot support a diagnosis",
          "[analysis][pipeline][graphics][validation]") {
    const auto base = fixture::incident(std::nullopt);
    const auto header = base->header();
    const blackbox::core::SystemEvent mismatched{
        .observed_at = header.window.event_time,
        .source = blackbox::core::SystemEventSource::application,
        .kind = blackbox::core::SystemEventKind::display_driver_recovery,
        .level = blackbox::core::SystemEventLevel::warning,
        .native_event_id = 4101U};
    const blackbox::core::IncidentSnapshot input{
        header,
        {base->system_samples().begin(), base->system_samples().end()},
        {base->process_metadata().begin(), base->process_metadata().end()},
        {base->process_samples().begin(), base->process_samples().end()},
        {mismatched}};

    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(input);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->diagnosis.available);
}

TEST_CASE("storage retry is a precise automatic Windows symptom",
          "[analysis][pipeline][storage][system-event]") {
    const auto base = fixture::incident(std::nullopt);
    auto header = base->header();
    header.window.automatic_trigger_count = 1U;
    header.window.manual_trigger_count = 0U;
    header.window.automatic_resource = blackbox::core::AutomaticIncidentResource::disk;
    header.window.automatic_score = 1.0;
    header.window.automatic_signal =
        blackbox::core::AutomaticIncidentSignal::storage_io_retry;
    const blackbox::core::SystemEvent retry{
        .observed_at = header.window.event_time,
        .source = blackbox::core::SystemEventSource::storage,
        .kind = blackbox::core::SystemEventKind::storage_io_retry,
        .level = blackbox::core::SystemEventLevel::warning,
        .native_event_id = 153U};
    const blackbox::core::IncidentSnapshot input{
        header,
        {base->system_samples().begin(), base->system_samples().end()},
        {base->process_metadata().begin(), base->process_metadata().end()},
        {base->process_samples().begin(), base->process_samples().end()},
        {retry}};

    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(input);
    REQUIRE(result.has_value());
    CHECK(result->diagnosis.available);
    CHECK(result->diagnosis.type == analysis::IncidentType::storage_io_retry);
    CHECK(result->diagnosis.basis ==
          analysis::SymptomExplanationBasis::automatic_capture_alignment);
    CHECK(result->diagnosis.calibrated_confidence == 0.98);
    CHECK(std::any_of(result->diagnosis.evidence.begin(),
                      result->diagnosis.evidence.end(), [](const auto& link) {
        return link.kind == analysis::DiagnosisEvidenceKind::system_event;
    }));
    CHECK(std::any_of(result->diagnosis.evidence.begin(),
                      result->diagnosis.evidence.end(), [](const auto& link) {
        return link.kind == analysis::DiagnosisEvidenceKind::automatic_capture_trigger;
    }));
    CHECK(analysis::diagnosis_evidence_links_valid(input, *result));
}

TEST_CASE("mismatched storage retry identity and resource cannot support a diagnosis",
          "[analysis][pipeline][storage][validation]") {
    const auto base = fixture::incident(std::nullopt);
    auto header = base->header();
    header.window.automatic_trigger_count = 1U;
    header.window.manual_trigger_count = 0U;
    header.window.automatic_resource = blackbox::core::AutomaticIncidentResource::none;
    header.window.automatic_signal =
        blackbox::core::AutomaticIncidentSignal::storage_io_retry;
    const blackbox::core::SystemEvent mismatched{
        .observed_at = header.window.event_time,
        .source = blackbox::core::SystemEventSource::device,
        .kind = blackbox::core::SystemEventKind::storage_io_retry,
        .level = blackbox::core::SystemEventLevel::warning,
        .native_event_id = 153U};
    const blackbox::core::IncidentSnapshot input{
        header,
        {base->system_samples().begin(), base->system_samples().end()},
        {base->process_metadata().begin(), base->process_metadata().end()},
        {base->process_samples().begin(), base->process_samples().end()},
        {mismatched}};

    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(input);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->diagnosis.available);
}

TEST_CASE("nearby DNS timeout is a precise Windows-reported symptom not a root-cause claim",
          "[analysis][pipeline][dns][system-event]") {
    const auto base = fixture::incident(std::nullopt);
    const auto header = base->header();
    const blackbox::core::SystemEvent dns_timeout{
        .observed_at = header.window.event_time,
        .source = blackbox::core::SystemEventSource::network,
        .kind = blackbox::core::SystemEventKind::dns_resolution_timeout,
        .level = blackbox::core::SystemEventLevel::warning,
        .native_event_id = 1014U};
    const blackbox::core::IncidentSnapshot input{
        header,
        {base->system_samples().begin(), base->system_samples().end()},
        {base->process_metadata().begin(), base->process_metadata().end()},
        {base->process_samples().begin(), base->process_samples().end()},
        {dns_timeout}};

    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(input);
    REQUIRE(result.has_value());
    CHECK(result->diagnosis.available);
    CHECK(result->diagnosis.type == analysis::IncidentType::dns_resolution_timeout);
    CHECK(result->diagnosis.basis ==
          analysis::SymptomExplanationBasis::system_event_alignment);
    CHECK(result->diagnosis.calibrated_confidence == 0.88);
    CHECK(result->diagnosis.primary_contributor_index == std::nullopt);
    REQUIRE(result->diagnosis.evidence.size() == 1U);
    CHECK(result->diagnosis.evidence.front().kind ==
          analysis::DiagnosisEvidenceKind::system_event);
    CHECK(analysis::diagnosis_evidence_links_valid(input, *result));
}

TEST_CASE("distant DNS timeout remains context and cannot explain the incident",
          "[analysis][pipeline][dns][abstention]") {
    const auto base = fixture::incident(std::nullopt);
    const auto header = base->header();
    const blackbox::core::SystemEvent dns_timeout{
        .observed_at = header.window.event_time + std::chrono::seconds{6},
        .source = blackbox::core::SystemEventSource::network,
        .kind = blackbox::core::SystemEventKind::dns_resolution_timeout,
        .level = blackbox::core::SystemEventLevel::warning,
        .native_event_id = 1014U};
    const blackbox::core::IncidentSnapshot input{
        header,
        {base->system_samples().begin(), base->system_samples().end()},
        {base->process_metadata().begin(), base->process_metadata().end()},
        {base->process_samples().begin(), base->process_samples().end()},
        {dns_timeout}};

    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(input);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->diagnosis.available);
    CHECK(result->diagnosis.type == analysis::IncidentType::unknown);
}

TEST_CASE("mismatched DNS event identity cannot support a diagnosis",
          "[analysis][pipeline][dns][validation]") {
    const auto base = fixture::incident(std::nullopt);
    const auto header = base->header();
    const blackbox::core::SystemEvent mismatched{
        .observed_at = header.window.event_time,
        .source = blackbox::core::SystemEventSource::application,
        .kind = blackbox::core::SystemEventKind::dns_resolution_timeout,
        .level = blackbox::core::SystemEventLevel::warning,
        .native_event_id = 1014U};
    const blackbox::core::IncidentSnapshot input{
        header,
        {base->system_samples().begin(), base->system_samples().end()},
        {base->process_metadata().begin(), base->process_metadata().end()},
        {base->process_samples().begin(), base->process_samples().end()},
        {mismatched}};

    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(input);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->diagnosis.available);
}

TEST_CASE("aligned resource diagnosis outranks a coincidental DNS timeout",
          "[analysis][pipeline][dns][priority]") {
    const auto base = fixture::incident(analysis::ResourceKind::cpu,
                                        std::nullopt, true);
    const auto header = base->header();
    const blackbox::core::SystemEvent dns_timeout{
        .observed_at = header.window.event_time,
        .source = blackbox::core::SystemEventSource::network,
        .kind = blackbox::core::SystemEventKind::dns_resolution_timeout,
        .level = blackbox::core::SystemEventLevel::warning,
        .native_event_id = 1014U};
    const blackbox::core::IncidentSnapshot input{
        header,
        {base->system_samples().begin(), base->system_samples().end()},
        {base->process_metadata().begin(), base->process_metadata().end()},
        {base->process_samples().begin(), base->process_samples().end()},
        {dns_timeout}};

    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(input);
    REQUIRE(result.has_value());
    CHECK(result->diagnosis.available);
    CHECK(result->diagnosis.type == analysis::IncidentType::cpu_pressure);
    CHECK(result->diagnosis.basis ==
          analysis::SymptomExplanationBasis::automatic_capture_alignment);
}

TEST_CASE("automatic recurrence contributes bounded evidence but user grouping does not",
          "[analysis][pipeline][recurrence][calibration]") {
    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto input = fixture::incident(analysis::ResourceKind::disk);
    const analysis::IncidentRecurrenceContext automatic{
        true, true, false, 4U, 3U, 0.90, 0.05};
    const analysis::IncidentAnalysisContext automatic_context{
        42, 2'000, {}, automatic};
    const auto recurring = analyzer.analyze(*input, automatic_context);
    REQUIRE(recurring.has_value());
    CHECK(recurring->recurrence == automatic);
    CHECK(std::any_of(recurring->diagnosis.evidence.begin(),
                      recurring->diagnosis.evidence.end(), [](const auto& link) {
        return link.kind == analysis::DiagnosisEvidenceKind::recurring_pattern;
    }));

    auto manual = automatic;
    manual.manually_overridden = true;
    const auto overridden = analyzer.analyze(
        *input, analysis::IncidentAnalysisContext{42, 2'000, {}, manual});
    REQUIRE(overridden.has_value());
    CHECK_FALSE(std::any_of(overridden->diagnosis.evidence.begin(),
                            overridden->diagnosis.evidence.end(), [](const auto& link) {
        return link.kind == analysis::DiagnosisEvidenceKind::recurring_pattern;
    }));
    CHECK(recurring->diagnosis.calibrated_confidence >
          overridden->diagnosis.calibrated_confidence);
}

TEST_CASE("confirmed similar incidents are inspectable without changing current analysis",
          "[analysis][pipeline][similar-incidents][feedback]") {
    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto input = fixture::incident(analysis::ResourceKind::disk);
    const analysis::IncidentRecurrenceContext automatic{
        true, true, false, 4U, 3U, 0.90, 0.05};
    const std::vector<analysis::SimilarIncidentFeedbackObservation> history{
        {39, 1'700, analysis::SimilarIncidentSymptom::game_stutter,
         analysis::SimilarIncidentFeedback::problem_confirmed},
        {40, 1'800, analysis::SimilarIncidentSymptom::game_stutter,
         analysis::SimilarIncidentFeedback::problem_confirmed},
    };
    const auto without_history = analyzer.analyze(
        *input, analysis::IncidentAnalysisContext{42, 2'000, {}, automatic});
    const auto with_history = analyzer.analyze(
        *input, analysis::IncidentAnalysisContext{
                    42, 2'000, {}, automatic, {}, 0, 0, false, history});

    REQUIRE(without_history.has_value());
    REQUIRE(with_history.has_value());
    CHECK(with_history->similar_incident_evidence.state ==
          analysis::SimilarIncidentEvidenceState::ready);
    CHECK(with_history->diagnosis == without_history->diagnosis);
    CHECK(with_history->resources == without_history->resources);
    CHECK(with_history->processes == without_history->processes);
    CHECK(with_history->contributors == without_history->contributors);
}

TEST_CASE("correlated contributor evidence is penalized and remains a reference",
          "[analysis][pipeline][contributor][calibration]") {
    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto input = fixture::incident(analysis::ResourceKind::cpu);
    const auto result = analyzer.analyze(*input);
    REQUIRE(result.has_value());
    REQUIRE(result->diagnosis.primary_contributor_index.has_value());
    CHECK(result->diagnosis.correlated_evidence_penalty > 0.0);
    CHECK(std::any_of(result->diagnosis.evidence.begin(),
                      result->diagnosis.evidence.end(), [](const auto& link) {
        return link.kind == analysis::DiagnosisEvidenceKind::process_anomaly &&
               link.confidence_contribution == 0.0;
    }));
}

TEST_CASE("statistical components are identical with native ML not adopted",
          "[analysis][pipeline][native-ml][fallback]") {
    const auto input = fixture::incident(analysis::ResourceKind::memory);
    analysis::PersonalizedProcessAnalyzer components;
    analysis::IntelligentIncidentAnalyzer pipeline;
    const auto component_result = components.analyze(*input);
    const auto pipeline_result = pipeline.analyze(*input);
    REQUIRE(component_result.has_value());
    REQUIRE(pipeline_result.has_value());
    CHECK(pipeline_result->resources == component_result->resources);
    CHECK(pipeline_result->processes == component_result->processes);
    CHECK(pipeline_result->contributors == component_result->contributors);
    CHECK(pipeline_result->workload_context == component_result->workload_context);
    CHECK(pipeline_result->provenance.native_inference ==
          analysis::NativeInferenceStatus::not_adopted);
}

TEST_CASE("component failures propagate without fabricating a diagnosis",
          "[analysis][pipeline][failure]") {
    auto input = fixture::incident(analysis::ResourceKind::cpu);
    auto header = input->header();
    header.actual_end = blackbox::core::MonotonicTimePoint{
        std::chrono::nanoseconds{0}};
    header.actual_start = blackbox::core::MonotonicTimePoint{
        std::chrono::nanoseconds{1}};
    const blackbox::core::IncidentSnapshot invalid{
        header, {}, {}, {}};
    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(invalid);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == analysis::AnalysisErrorCode::invalid_incident);
}

TEST_CASE("diagnosis evidence validation rejects dangling references",
          "[analysis][pipeline][evidence][failure]") {
    const auto input = fixture::incident(
        analysis::ResourceKind::network, std::nullopt, true);
    analysis::IntelligentIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(*input);
    REQUIRE(result.has_value());

    auto corrupted = *result;
    const auto resource = std::find_if(
        corrupted.diagnosis.evidence.begin(), corrupted.diagnosis.evidence.end(),
        [](const auto& link) {
            return link.kind == analysis::DiagnosisEvidenceKind::resource_anomaly;
        });
    REQUIRE(resource != corrupted.diagnosis.evidence.end());
    resource->source_index = corrupted.resources.size();
    CHECK_FALSE(analysis::diagnosis_evidence_links_valid(*input, corrupted));
}
