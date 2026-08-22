#include "app/incident_viewer_service.hpp"
#include "storage/test_incident.hpp"
#include "telemetry/collector.hpp"
#include "telemetry/mock/mock_telemetry_provider.hpp"
#if BLACKBOX_ANALYSIS_ENABLED
#include "analysis/diagnosis_fixture.hpp"
#include "analysis/intelligent_incident_analyzer.hpp"
#include "analysis/personalized_process_analyzer.hpp"
#include "analysis/statistical_incident_analyzer.hpp"
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <expected>
#include <mutex>
#include <optional>
#include <thread>

namespace app = blackbox::app;
namespace core = blackbox::core;
namespace storage = blackbox::storage;
namespace ui = blackbox::ui;
namespace telemetry = blackbox::telemetry;
namespace mock = blackbox::telemetry::mock;
#if BLACKBOX_ANALYSIS_ENABLED
namespace analysis = blackbox::analysis;
namespace diagnosis_fixture = blackbox::test::diagnosis_fixture;
#endif
using namespace std::chrono_literals;

namespace {

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate,
                              const std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

class ViewerRepository final : public storage::IIncidentRepository,
                               public storage::IProcessProfileRepository,
                               public storage::IFeedbackCalibrationRepository,
                               public storage::IRecurringIncidentRepository {
public:
    std::expected<std::int64_t, storage::StorageError> store(
        const core::IncidentSnapshot&) noexcept override {
        return 1;
    }
    std::expected<storage::StoredIncidentPage, storage::StorageError> list_page(
        const storage::IncidentListQuery& query) const noexcept override {
        record_call();
        if (delay.count() != 0) std::this_thread::sleep_for(delay);
        storage::StoredIncidentSummary summary{};
        summary.id = 1;
        summary.created_utc_milliseconds = 0;
        summary.actual_start_nanoseconds = 0;
        summary.actual_end_nanoseconds = 2'000'000'000;
        summary.system_sample_count = 2U;
        summary.process_sample_count = 1U;
        summary.label = annotation_value.label;
        summary.note = annotation_value.note;
        return storage::StoredIncidentPage{{std::move(summary)}, 1U, query.offset};
    }
    std::expected<std::shared_ptr<const core::IncidentSnapshot>, storage::StorageError>
    load(const std::int64_t) const noexcept override {
        record_call();
        return incident_value;
    }
    std::expected<storage::IncidentAnnotation, storage::StorageError> annotation(
        const std::int64_t) const noexcept override {
        record_call();
        const std::scoped_lock lock{mutex};
        return annotation_value;
    }
    std::expected<void, storage::StorageError> update_annotation(
        const std::int64_t, const storage::IncidentAnnotation& value) noexcept override {
        record_call();
        const std::scoped_lock lock{mutex};
        annotation_value = value;
        return {};
    }
    std::expected<storage::ProcessProfileContext, storage::StorageError>
    process_profile_context(
        const std::int64_t incident_id,
        const std::span<const std::string>) const noexcept override {
        record_call();
        const std::scoped_lock lock{mutex};
        ++profile_loads;
        return storage::ProcessProfileContext{
            incident_id, 1'800'000'000'000LL, profile_history};
    }
    std::expected<void, storage::StorageError> store_process_profile_updates(
        const std::int64_t,
        const std::span<const storage::ProcessProfileUpdate> updates) noexcept override {
        record_call();
        const std::scoped_lock lock{mutex};
        ++profile_stores;
        stored_profile_updates.assign(updates.begin(), updates.end());
        return {};
    }
    std::expected<storage::FeedbackCalibrationContext, storage::StorageError>
    feedback_calibration_context(
        const std::int64_t incident_id,
        const std::size_t maximum_observations) const noexcept override {
        record_call();
        const std::scoped_lock lock{mutex};
        ++feedback_loads;
        auto history = feedback_history;
        std::erase_if(history, [this](const auto& observation) {
            return observation.observed_utc_milliseconds <= feedback_reset_after;
        });
        if (history.size() > maximum_observations) {
            history.resize(maximum_observations);
        }
        return storage::FeedbackCalibrationContext{
            incident_id, 1'800'000'000'000LL, feedback_revision,
            feedback_reset_after, feedback_rollback_available,
            std::move(history)};
    }
    std::expected<storage::FeedbackProfileControlState, storage::StorageError>
    reset_feedback_profile() noexcept override {
        const std::scoped_lock lock{mutex};
        feedback_previous_reset_after = feedback_reset_after;
        feedback_reset_after = 1'800'000'000'001LL;
        feedback_rollback_available = true;
        ++feedback_revision;
        return storage::FeedbackProfileControlState{
            feedback_revision, feedback_reset_after, true};
    }
    std::expected<storage::FeedbackProfileControlState, storage::StorageError>
    rollback_feedback_profile_reset() noexcept override {
        const std::scoped_lock lock{mutex};
        if (!feedback_rollback_available) {
            return std::unexpected{storage::StorageError{
                storage::StorageErrorCode::invalid_data, 0,
                "no feedback reset available"}};
        }
        feedback_reset_after = feedback_previous_reset_after;
        feedback_rollback_available = false;
        ++feedback_revision;
        return storage::FeedbackProfileControlState{
            feedback_revision, feedback_reset_after, false};
    }
    std::expected<storage::ContributorFeedbackContext, storage::StorageError>
    contributor_feedback_context(
        const std::int64_t incident_id,
        const std::span<const std::string> executable_keys,
        const std::size_t maximum_observations) const noexcept override {
        record_call();
        const std::scoped_lock lock{mutex};
        const auto accepts = [&](const auto& observation) {
            return std::find(executable_keys.begin(), executable_keys.end(),
                             observation.executable_key) != executable_keys.end();
        };
        storage::ContributorFeedbackContext result{};
        result.incident_id = incident_id;
        result.incident_utc_milliseconds = 1'800'000'000'000LL;
        std::copy_if(contributor_feedback_current.begin(),
                     contributor_feedback_current.end(),
                     std::back_inserter(result.current), accepts);
        std::copy_if(contributor_feedback_history.begin(),
                     contributor_feedback_history.end(),
                     std::back_inserter(result.history), accepts);
        if (result.history.size() > maximum_observations)
            result.history.resize(maximum_observations);
        return result;
    }
    std::expected<void, storage::StorageError> update_contributor_feedback(
        const std::int64_t incident_id, std::string executable_key,
        const storage::ContributorFeedbackResource resource,
        const storage::ContributorFeedbackDisposition disposition,
        const storage::ContributorFeedbackTemporalRelationship
            temporal_relationship) noexcept override {
        record_call();
        const std::scoped_lock lock{mutex};
        std::erase_if(contributor_feedback_current, [&](const auto& observation) {
            return observation.incident_id == incident_id &&
                   observation.executable_key == executable_key &&
                   observation.resource == resource;
        });
        if (disposition != storage::ContributorFeedbackDisposition::unsure) {
            contributor_feedback_current.push_back(
                storage::StoredContributorFeedbackObservation{
                    incident_id, 1'800'000'000'000LL,
                    1'800'000'000'001LL, std::move(executable_key), resource,
                    disposition, temporal_relationship});
        }
        ++contributor_feedback_stores;
        return {};
    }
    std::expected<std::vector<storage::StoredRecurringIncident>, storage::StorageError>
    recurring_incidents(const std::size_t maximum_results) const noexcept override {
        record_call();
        const std::scoped_lock lock{mutex};
        auto result = recurring_records;
        if (result.size() > maximum_results) result.resize(maximum_results);
        return result;
    }
    std::expected<std::string, storage::StorageError> recurring_group_override(
        const std::int64_t incident_id) const noexcept override {
        record_call();
        const std::scoped_lock lock{mutex};
        const auto found = std::find_if(recurring_records.begin(), recurring_records.end(),
                                        [incident_id](const auto& record) {
            return record.id == incident_id;
        });
        return found == recurring_records.end() ? std::string{} : found->override_group;
    }
    std::expected<void, storage::StorageError> store_incident_features(
        const std::span<const storage::StoredIncidentFeatureCache> features) noexcept override {
        record_call();
        const std::scoped_lock lock{mutex};
        ++feature_stores;
        for (const auto& feature : features) {
            const auto found = std::find_if(recurring_records.begin(), recurring_records.end(),
                                            [&](const auto& record) {
                return record.id == feature.incident_id;
            });
            if (found != recurring_records.end()) found->cached_feature = feature;
        }
        return {};
    }
    std::expected<void, storage::StorageError> update_recurring_group_override(
        const std::int64_t incident_id, std::string override_group) noexcept override {
        record_call();
        const std::scoped_lock lock{mutex};
        const auto found = std::find_if(recurring_records.begin(), recurring_records.end(),
                                        [incident_id](const auto& record) {
            return record.id == incident_id;
        });
        if (found != recurring_records.end())
            found->override_group = std::move(override_group);
        ++override_stores;
        return {};
    }

    void record_call() const noexcept {
        const std::scoped_lock lock{mutex};
        database_thread = std::this_thread::get_id();
        ++calls;
    }

    mutable std::mutex mutex{};
    mutable std::thread::id database_thread{};
    mutable std::uint64_t calls{};
    mutable std::uint64_t profile_loads{};
    mutable std::uint64_t feedback_loads{};
    std::uint64_t feedback_revision{};
    std::int64_t feedback_reset_after{};
    std::int64_t feedback_previous_reset_after{};
    bool feedback_rollback_available{};
    std::uint64_t profile_stores{};
    std::vector<storage::StoredProcessProfileObservation> profile_history{};
    std::vector<storage::StoredFeedbackCalibrationObservation> feedback_history{};
    mutable std::vector<storage::StoredContributorFeedbackObservation>
        contributor_feedback_current{};
    std::vector<storage::StoredContributorFeedbackObservation>
        contributor_feedback_history{};
    std::uint64_t contributor_feedback_stores{};
    std::vector<storage::ProcessProfileUpdate> stored_profile_updates{};
    mutable std::vector<storage::StoredRecurringIncident> recurring_records{};
    std::uint64_t feature_stores{};
    std::uint64_t override_stores{};
    storage::IncidentAnnotation annotation_value{};
    std::shared_ptr<const core::IncidentSnapshot> incident_value{
        storage::test::representative_incident()};
    std::chrono::milliseconds delay{};
};

#if BLACKBOX_ANALYSIS_ENABLED
class RecordingAnalyzer final : public analysis::IIncidentAnalyzer {
public:
    std::expected<analysis::IncidentAnalysis, analysis::AnalysisError> analyze(
        const core::IncidentSnapshot& incident) const noexcept override {
        {
            const std::scoped_lock lock{mutex};
            analysis_thread = std::this_thread::get_id();
            ++calls;
        }
        return implementation.analyze(incident);
    }

    analysis::StatisticalIncidentAnalyzer implementation{};
    mutable std::mutex mutex{};
    mutable std::thread::id analysis_thread{};
    mutable std::uint64_t calls{};
};

class ContributorAnalyzer final : public analysis::IIncidentAnalyzer {
public:
    std::expected<analysis::IncidentAnalysis, analysis::AnalysisError> analyze(
        const core::IncidentSnapshot& incident) const noexcept override {
        analysis::IncidentAnalysis result{};
        result.baseline_start = incident.header().actual_start;
        result.baseline_end = incident.header().window.event_time - 30s;
        result.evaluation_start = result.baseline_end;
        result.evaluation_end = incident.header().actual_end;
        analysis::ContributorCandidate candidate{};
        candidate.identity = {77U, 88U};
        candidate.name = "preceding.exe";
        if (!incident.process_metadata().empty()) {
            if (const auto executable = analysis::normalize_executable_identity(
                    incident.process_metadata().front())) {
                candidate.executable_key = executable->key;
            }
        }
        candidate.strength = analysis::ContributorStrength::likely;
        candidate.temporal_relationship =
            analysis::ContributorTemporalRelationship::preceding_activity;
        candidate.confidence = analysis::AnalysisConfidence::high;
        candidate.score = 0.91;
        candidate.anomaly_magnitude = 1.0;
        candidate.timing_score = 1.0;
        candidate.resource_match_score = 0.9;
        candidate.duration_score = 0.8;
        candidate.recurrence_score = 0.5;
        candidate.evidence_coverage = 0.75;
        candidate.activity_started_seconds_from_event = -4.0;
        candidate.has_process_start_event = true;
        candidate.process_started_seconds_from_event = -5.0;
        candidate.has_process_exit_event = true;
        candidate.process_exited_seconds_from_event = 3.0;
        candidate.anomalous_duration_seconds = 3.0;
        candidate.pre_marker_anomalous_samples = 4U;
        candidate.post_marker_anomalous_samples = 1U;
        candidate.recurrence_count = 2U;
        candidate.missing_metrics = 1U;
        result.contributors.push_back(candidate);
        auto ambiguous = candidate;
        ambiguous.identity = {78U, 89U};
        ambiguous.name = "spanning.exe";
        ambiguous.executable_key = "name:spanning.exe";
        ambiguous.strength = analysis::ContributorStrength::potential;
        ambiguous.temporal_relationship =
            analysis::ContributorTemporalRelationship::marker_spanning_ambiguous;
        ambiguous.score = 0.60;
        ambiguous.timing_score = 0.35;
        ambiguous.activity_started_seconds_from_event = -1.0;
        ambiguous.pre_marker_anomalous_samples = 2U;
        ambiguous.post_marker_anomalous_samples = 8U;
        result.contributors.push_back(std::move(ambiguous));
        auto reaction = candidate;
        reaction.identity = {79U, 90U};
        reaction.name = "reaction.exe";
        reaction.executable_key = "name:reaction.exe";
        reaction.strength = analysis::ContributorStrength::potential;
        reaction.temporal_relationship =
            analysis::ContributorTemporalRelationship::post_marker_reaction;
        reaction.score = 0.40;
        reaction.timing_score = 0.15;
        reaction.activity_started_seconds_from_event = 2.0;
        reaction.pre_marker_anomalous_samples = 0U;
        reaction.post_marker_anomalous_samples = 5U;
        result.contributors.push_back(std::move(reaction));
        return result;
    }
};
#endif

} // namespace

TEST_CASE("viewer performs repository work off the caller thread and publishes bounded views",
          "[app][viewer][threading]") {
    ViewerRepository repository;
    app::IncidentViewerService viewer{repository};
    const auto caller_thread = std::this_thread::get_id();
    viewer.start();
    viewer.request_page(0U, {}, ui::IncidentListOrder::newest_first);
    REQUIRE(wait_until([&] { return viewer.snapshot()->generation >= 1U; }));
    REQUIRE(viewer.snapshot()->incidents.size() == 1U);
    {
        const std::scoped_lock lock{repository.mutex};
        CHECK(repository.database_thread != caller_thread);
    }

    viewer.request_detail(1);
    REQUIRE(wait_until([&] {
        const auto state = viewer.snapshot();
        return state->generation >= 2U && state->detail.has_value();
    }));
    CHECK(viewer.snapshot()->detail->analysis.state ==
          ui::IncidentAnalysisViewState::disabled);
    const auto identity = viewer.snapshot()->detail->processes.front().identity;
    viewer.request_process(1, identity);
    REQUIRE(wait_until([&] {
        const auto state = viewer.snapshot();
        return state->generation >= 3U && state->detail->selected_process.has_value();
    }));
    viewer.update_annotation(1, "Game", "stutter",
                             storage::IncidentUserFeedback::noticed_problem,
                             storage::IncidentCategory::game_stutter);
    REQUIRE(wait_until([&] {
        const auto state = viewer.snapshot();
        return state->generation >= 4U && state->detail->label == "Game";
    }));
    viewer.stop();
    CHECK(viewer.snapshot()->detail->user_feedback ==
          ui::IncidentFeedback::noticed_problem);
    CHECK(viewer.snapshot()->detail->category == ui::IncidentCategory::game_stutter);
    const storage::IncidentAnnotation expected{
        "Game", "stutter", storage::IncidentUserFeedback::noticed_problem,
        storage::IncidentCategory::game_stutter};
    CHECK(repository.annotation_value == expected);
}

#if BLACKBOX_ANALYSIS_ENABLED
TEST_CASE("viewer caches bounded recurring features and publishes inspectable groups",
          "[app][viewer][recurring][threading]") {
    ViewerRepository repository;
    repository.recurring_records = {
        storage::StoredRecurringIncident{1, 1'000, "first", {}, std::nullopt},
        storage::StoredRecurringIncident{2, 2'000, "second", {}, std::nullopt}};
    app::IncidentViewerService viewer{
        repository, nullptr, nullptr, nullptr, &repository};
    const auto caller_thread = std::this_thread::get_id();
    viewer.start();
    viewer.request_recurring_incidents();
    REQUIRE(wait_until([&] {
        return viewer.snapshot()->recurring.state ==
               ui::RecurringIncidentViewState::ready;
    }));
    auto recurring = viewer.snapshot()->recurring;
    CHECK(recurring.incidents_considered == 2U);
    CHECK(recurring.recomputed_features == 2U);
    CHECK(recurring.cached_features == 0U);
    REQUIRE(recurring.groups.size() == 1U);
    CHECK(recurring.groups.front().members.size() == 2U);
    CHECK_FALSE(recurring.groups.front().shared_evidence.empty());
    const auto first_generation = viewer.snapshot()->generation;

    viewer.request_recurring_incidents();
    REQUIRE(wait_until([&] {
        const auto snapshot = viewer.snapshot();
        return snapshot->generation > first_generation &&
               snapshot->recurring.cached_features == 2U;
    }));
    CHECK(viewer.snapshot()->recurring.recomputed_features == 0U);
    {
        const std::scoped_lock lock{repository.mutex};
        CHECK(repository.feature_stores == 1U);
        CHECK(repository.database_thread != caller_thread);
    }

    viewer.request_detail(1);
    REQUIRE(wait_until([&] { return viewer.snapshot()->detail.has_value(); }));
    viewer.update_recurring_group_override(1, "my recurring issue");
    REQUIRE(wait_until([&] {
        const auto snapshot = viewer.snapshot();
        return snapshot->detail &&
               snapshot->detail->recurring_group_override == "my recurring issue" &&
               !snapshot->recurring.groups.empty() &&
               snapshot->recurring.groups.front().manually_overridden;
    }));
    CHECK(viewer.snapshot()->recurring.groups.front().name.find("my recurring issue") !=
          std::string::npos);
    const auto override_generation = viewer.snapshot()->generation;
    viewer.update_recurring_group_override(1, {});
    REQUIRE(wait_until([&] {
        const auto snapshot = viewer.snapshot();
        return snapshot->generation > override_generation && snapshot->detail &&
               snapshot->detail->recurring_group_override.empty() &&
               snapshot->recurring.groups.size() == 1U &&
               !snapshot->recurring.groups.front().manually_overridden;
    }));
    viewer.stop();
    CHECK(repository.override_stores == 2U);
}

TEST_CASE("viewer refreshes a versioned diagnosis with recurring evidence",
          "[app][viewer][analysis][pipeline][recurring]") {
    ViewerRepository repository;
    repository.incident_value = diagnosis_fixture::incident(
        analysis::ResourceKind::cpu);
    repository.recurring_records = {
        storage::StoredRecurringIncident{1, 1'000, "first", {}, std::nullopt},
        storage::StoredRecurringIncident{2, 2'000, "second", {}, std::nullopt}};
    analysis::IntelligentIncidentAnalyzer analyzer;
    app::IncidentViewerService viewer{
        repository, &analyzer, &repository, &repository, &repository};
    viewer.start();
    viewer.request_detail(1);
    REQUIRE(wait_until([&] {
        const auto snapshot = viewer.snapshot();
        return snapshot->detail &&
               snapshot->detail->analysis.diagnosis.available;
    }));
    CHECK(viewer.snapshot()->detail->analysis.diagnosis.pipeline_version ==
          analysis::intelligent_pipeline_version);

    viewer.request_recurring_incidents();
    REQUIRE(wait_until([&] {
        const auto snapshot = viewer.snapshot();
        if (!snapshot->detail || snapshot->recurring.groups.empty()) return false;
        return std::any_of(
            snapshot->detail->analysis.diagnosis.evidence.begin(),
            snapshot->detail->analysis.diagnosis.evidence.end(),
            [](const auto& evidence) {
                return evidence.find("Automatic recurring pattern") !=
                       std::string::npos;
            });
    }));
    viewer.stop();
    const auto& diagnosis = viewer.snapshot()->detail->analysis.diagnosis;
    CHECK(diagnosis.incident_type == "CPU pressure pattern");
    CHECK_FALSE(diagnosis.primary_contributor.empty());
    CHECK(diagnosis.inference.find("native ML not adopted") != std::string::npos);
    CHECK(diagnosis.configuration_fingerprint != 0U);
}

TEST_CASE("viewer reuses confirmed automatic recurrence as resettable context only",
          "[app][viewer][analysis][similar-incidents][feedback]") {
    ViewerRepository repository;
    repository.incident_value = diagnosis_fixture::incident(
        analysis::ResourceKind::cpu);
    constexpr auto current = 1'800'000'000'000LL;
    storage::StoredRecurringIncident first{
        1, current - 2'000, "first", {}, std::nullopt};
    first.user_feedback = storage::IncidentUserFeedback::noticed_problem;
    first.category = storage::IncidentCategory::game_stutter;
    storage::StoredRecurringIncident second{
        2, current - 1'000, "second", {}, std::nullopt};
    second.user_feedback = storage::IncidentUserFeedback::noticed_problem;
    second.category = storage::IncidentCategory::game_stutter;
    storage::StoredRecurringIncident present{
        3, current, "present", {}, std::nullopt};
    repository.recurring_records = {first, second, present};

    analysis::IntelligentIncidentAnalyzer analyzer;
    app::IncidentViewerService viewer{
        repository, &analyzer, &repository, &repository, &repository};
    viewer.start();
    viewer.request_detail(3);
    REQUIRE(wait_until([&] {
        return viewer.snapshot()->detail.has_value();
    }));
    viewer.request_recurring_incidents();
    REQUIRE(wait_until([&] {
        const auto snapshot = viewer.snapshot();
        return snapshot->detail &&
               snapshot->detail->analysis.similar_incidents.ready;
    }));
    auto analysis_view = viewer.snapshot()->detail->analysis;
    CHECK(analysis_view.similar_incidents.symptom == "Game stutter");
    CHECK(analysis_view.similar_incidents.matching_confirmations == 2U);
    const auto confidence_with_context =
        analysis_view.diagnosis.calibrated_confidence;
    const auto contributor_with_context =
        analysis_view.diagnosis.primary_contributor;

    viewer.reset_feedback_profile();
    REQUIRE(wait_until([&] {
        const auto snapshot = viewer.snapshot();
        return snapshot->detail &&
               !snapshot->detail->analysis.similar_incidents.ready &&
               snapshot->detail->analysis.feedback.rollback_available;
    }));
    CHECK(viewer.snapshot()->detail->analysis.similar_incidents.status.find(
              "cold") != std::string::npos);
    CHECK(viewer.snapshot()->detail->analysis.diagnosis.calibrated_confidence ==
          confidence_with_context);
    CHECK(viewer.snapshot()->detail->analysis.diagnosis.primary_contributor ==
          contributor_with_context);

    viewer.rollback_feedback_profile_reset();
    REQUIRE(wait_until([&] {
        const auto snapshot = viewer.snapshot();
        return snapshot->detail &&
               snapshot->detail->analysis.similar_incidents.ready &&
               !snapshot->detail->analysis.feedback.rollback_available;
    }));
    viewer.stop();
}

TEST_CASE("viewer applies bounded prior feedback without touching current evidence",
          "[app][viewer][analysis][feedback]") {
    ViewerRepository repository;
    repository.incident_value = diagnosis_fixture::incident(
        analysis::ResourceKind::cpu, std::nullopt, true);
    for (std::int64_t id = 2; id <= 5; ++id) {
        repository.feedback_history.push_back(
            storage::StoredFeedbackCalibrationObservation{
                id, 1'799'999'000'000LL + id,
                core::AutomaticIncidentResource::cpu,
                core::AutomaticIncidentSignal::throughput_or_utilization,
                storage::IncidentUserFeedback::did_not_notice_problem});
    }
    auto configuration = analysis::IntelligentAnalysisConfiguration{};
    configuration.minimum_feedback_adjusted_assertion_confidence = 0.99;
    analysis::IntelligentIncidentAnalyzer analyzer{configuration};
    app::IncidentViewerService viewer{
        repository, &analyzer, &repository, &repository, nullptr};
    viewer.start();
    viewer.request_detail(1);
    REQUIRE(wait_until([&] {
        const auto snapshot = viewer.snapshot();
        return snapshot->detail &&
               snapshot->detail->analysis.diagnosis.pipeline_version != 0U;
    }));
    viewer.stop();
    const auto& analysis_view = viewer.snapshot()->detail->analysis;
    INFO(analysis_view.status);
    INFO(analysis_view.feedback.status);
    INFO(analysis_view.diagnosis.calibrated_confidence);
    INFO(analysis_view.diagnosis.feedback_multiplier);
    REQUIRE(analysis_view.diagnosis.suppressed_by_feedback);
    CHECK_FALSE(analysis_view.diagnosis.available);
    CHECK(analysis_view.feedback.applicable);
    CHECK(analysis_view.feedback.ready);
    CHECK(analysis_view.feedback.suppressing);
    CHECK(analysis_view.feedback.matching_observations == 4U);
    CHECK(analysis_view.feedback.false_positive_observations == 4U);
    CHECK(analysis_view.feedback.confidence_multiplier < 1.0);
    CHECK(analysis_view.feedback.status.find("reduced automatic-trigger confidence") !=
          std::string::npos);
    CHECK(repository.feedback_loads == 1U);

    viewer.start();
    viewer.reset_feedback_profile();
    REQUIRE(wait_until([&] {
        const auto snapshot = viewer.snapshot();
        return snapshot->detail &&
               snapshot->detail->analysis.feedback.profile_revision == 1U;
    }));
    const auto after_reset = viewer.snapshot()->detail->analysis;
    CHECK_FALSE(after_reset.feedback.suppressing);
    CHECK(after_reset.feedback.matching_observations == 0U);
    CHECK(after_reset.feedback.rollback_available);
    CHECK(after_reset.pressure.available);

    viewer.rollback_feedback_profile_reset();
    REQUIRE(wait_until([&] {
        const auto snapshot = viewer.snapshot();
        return snapshot->detail &&
               snapshot->detail->analysis.feedback.profile_revision == 2U;
    }));
    viewer.stop();
    const auto after_rollback = viewer.snapshot()->detail->analysis;
    CHECK(after_rollback.feedback.suppressing);
    CHECK(after_rollback.feedback.matching_observations == 4U);
    CHECK_FALSE(after_rollback.feedback.rollback_available);
    CHECK(after_rollback.pressure.available);
}

TEST_CASE("viewer separates observed pressure from an unavailable symptom explanation",
          "[app][viewer][analysis][pressure-separation]") {
    ViewerRepository repository;
    repository.incident_value = diagnosis_fixture::incident(
        analysis::ResourceKind::network);
    analysis::IntelligentIncidentAnalyzer analyzer;
    app::IncidentViewerService viewer{repository, &analyzer};
    viewer.start();
    viewer.request_detail(1);
    REQUIRE(wait_until([&] {
        const auto snapshot = viewer.snapshot();
        return snapshot->detail && snapshot->detail->analysis.pressure.available;
    }));
    viewer.stop();

    const auto& analysis_view = viewer.snapshot()->detail->analysis;
    CHECK(analysis_view.pressure.resource == "Network");
    CHECK(analysis_view.pressure.metric == "network receive");
    CHECK(analysis_view.pressure.score > 0.99);
    CHECK_FALSE(analysis_view.diagnosis.available);
    CHECK(analysis_view.diagnosis.basis == "No aligned symptom evidence");
}

TEST_CASE("viewer analyzes loaded incidents on its worker and exposes cold-start evidence",
          "[app][viewer][analysis][threading]") {
    ViewerRepository repository;
    RecordingAnalyzer analyzer;
    app::IncidentViewerService viewer{repository, &analyzer};
    const auto caller_thread = std::this_thread::get_id();
    viewer.start();
    viewer.request_detail(1);
    REQUIRE(wait_until([&] {
        const auto state = viewer.snapshot();
        return state->detail.has_value() &&
               state->detail->analysis.state ==
                   ui::IncidentAnalysisViewState::cold_start;
    }));
    viewer.stop();

    const std::scoped_lock lock{analyzer.mutex};
    CHECK(analyzer.calls == 1U);
    CHECK(analyzer.analysis_thread != caller_thread);
    CHECK(viewer.snapshot()->detail->analysis.status.find("Cold start") !=
          std::string::npos);
}

TEST_CASE("viewer exposes calibrated contributor wording and inspectable factors",
          "[app][viewer][analysis][contributor]") {
    ViewerRepository repository;
    ContributorAnalyzer analyzer;
    app::IncidentViewerService viewer{repository, &analyzer};
    viewer.start();
    viewer.request_detail(1);
    REQUIRE(wait_until([&] {
        return viewer.snapshot()->detail.has_value() &&
               !viewer.snapshot()->detail->analysis.contributors.empty();
    }));
    viewer.stop();
    const auto snapshot = viewer.snapshot();
    const auto& contributors = snapshot->detail->analysis.contributors;
    REQUIRE(contributors.size() == 3U);
    const auto& contributor = contributors.front();
    CHECK(contributor.assessment == "Likely contributor (correlation only)");
    CHECK(contributor.timing.find("before the marker") != std::string::npos);
    CHECK(contributor.timing.find("recorded process start -5.0 s") !=
          std::string::npos);
    CHECK(contributor.timing.find("recorded process exit +3.0 s") !=
          std::string::npos);
    CHECK(contributor.evidence.find("resource match") != std::string::npos);
    CHECK(contributor.evidence.find("missing metric") != std::string::npos);
    CHECK(contributor.assessment.find("proven") == std::string::npos);
    CHECK(contributors[1].assessment == "Ambiguous correlate across marker");
    CHECK(contributors[1].timing.find("most anomalous samples followed") !=
          std::string::npos);
    CHECK(contributors[1].temporal_relationship ==
          ui::IncidentContributorRow::TemporalRelationship::
              marker_spanning_ambiguous);
    CHECK(contributors[2].assessment ==
          "Possible victim/reaction (not a causal rank)");
    CHECK(contributors[2].timing.find("Possible victim/reaction") !=
          std::string::npos);
}

TEST_CASE("viewer persists explicit contributor attribution on its worker",
          "[app][viewer][analysis][contributor-feedback]") {
    ViewerRepository repository;
    ContributorAnalyzer analyzer;
    app::IncidentViewerService viewer{
        repository, &analyzer, nullptr, &repository};
    viewer.start();
    viewer.request_detail(1);
    REQUIRE(wait_until([&] {
        const auto content = viewer.snapshot();
        return content->detail &&
               !content->detail->analysis.contributors.empty();
    }));
    const auto initial = viewer.snapshot();
    const auto key = initial->detail->analysis.contributors.front().executable_key;
    REQUIRE_FALSE(key.empty());
    const auto generation = initial->generation;
    viewer.update_contributor_feedback(
        1, key, storage::ContributorFeedbackResource::cpu,
        storage::ContributorFeedbackDisposition::confirmed_contributor,
        storage::ContributorFeedbackTemporalRelationship::preceding_activity);
    REQUIRE(wait_until([&] {
        const auto content = viewer.snapshot();
        return content->generation > generation && content->detail &&
               !content->detail->analysis.contributors.empty() &&
               content->detail->analysis.contributors.front().attribution ==
                   ui::IncidentContributorRow::Attribution::
                       confirmed_contributor;
    }));
    viewer.stop();
    const std::scoped_lock lock{repository.mutex};
    CHECK(repository.contributor_feedback_stores == 1U);
    REQUIRE(repository.contributor_feedback_current.size() == 1U);
    CHECK(repository.contributor_feedback_current.front().temporal_relationship ==
          storage::ContributorFeedbackTemporalRelationship::preceding_activity);
}

TEST_CASE("collector continues while a large incident is statistically analyzed",
          "[app][viewer][analysis][collector]") {
    core::SystemMonotonicClock clock;
    mock::MockTelemetryProvider provider{clock};
    const auto configuration = telemetry::validate_recorder_configuration(
        telemetry::RecorderConfiguration{1ms, 20ms, 10ms});
    REQUIRE(configuration.has_value());
    telemetry::TelemetryCollector collector{provider, clock, *configuration};
    ViewerRepository repository;
    repository.incident_value = storage::test::scaled_incident(500U, 150U, false);
    analysis::StatisticalIncidentAnalyzer analyzer;
    app::IncidentViewerService viewer{repository, &analyzer};
    collector.start();
    viewer.start();
    REQUIRE(wait_until([&] { return collector.diagnostics().collection_count >= 5U; }));
    const auto before = collector.diagnostics().collection_count;
    viewer.request_detail(1);
    REQUIRE(wait_until([&] {
        const auto state = viewer.snapshot();
        return state->detail.has_value() &&
               state->detail->analysis.state == ui::IncidentAnalysisViewState::ready;
    }));
    const auto after = collector.diagnostics().collection_count;
    viewer.stop();
    collector.stop();

    CHECK(after >= before + 10U);
    CHECK(collector.diagnostics().failed_samples == 0U);
    REQUIRE_FALSE(viewer.snapshot()->detail->analysis.resources.empty());
    CHECK(viewer.snapshot()->detail->analysis.resources.front().score == 0.0);
    CHECK(viewer.snapshot()->detail->analysis.context.enabled);
    CHECK(viewer.snapshot()->detail->analysis.context.probabilities.size() == 8U);
    CHECK_FALSE(viewer.snapshot()->detail->analysis.context.primary.empty());
}

TEST_CASE("viewer maps personalized history and updates on its existing worker",
          "[app][viewer][analysis][personalized][threading]") {
    ViewerRepository repository;
    repository.incident_value = storage::test::scaled_incident(20U, 150U, false);
    analysis::PersonalizedProcessAnalyzer analyzer;
    app::IncidentViewerService viewer{repository, &analyzer, &repository};
    const auto caller_thread = std::this_thread::get_id();
    viewer.start();
    viewer.request_detail(1);
    REQUIRE(wait_until([&] {
        const auto state = viewer.snapshot();
        return state->detail.has_value() &&
               state->detail->analysis.status.find("profile") != std::string::npos;
    }));
    viewer.stop();
    const std::scoped_lock lock{repository.mutex};
    CHECK(repository.profile_loads == 1U);
    CHECK(repository.profile_stores == 1U);
    CHECK_FALSE(repository.stored_profile_updates.empty());
    CHECK(repository.database_thread != caller_thread);
}
#endif

TEST_CASE("collector continues while the viewer executes a slow archive query",
          "[app][viewer][collector]") {
    core::SystemMonotonicClock clock;
    mock::MockTelemetryProvider provider{clock};
    const auto configuration = telemetry::validate_recorder_configuration(
        telemetry::RecorderConfiguration{1ms, 20ms, 10ms});
    REQUIRE(configuration.has_value());
    telemetry::TelemetryCollector collector{provider, clock, *configuration};
    ViewerRepository repository;
    repository.delay = 75ms;
    app::IncidentViewerService viewer{repository};
    collector.start();
    viewer.start();
    REQUIRE(wait_until([&] { return collector.diagnostics().collection_count >= 5U; }));
    const auto before = collector.diagnostics().collection_count;
    viewer.request_page(0U, {}, ui::IncidentListOrder::newest_first);
    REQUIRE(wait_until([&] { return viewer.snapshot()->generation >= 1U; }));
    const auto after = collector.diagnostics().collection_count;
    viewer.stop();
    collector.stop();
    CHECK(after >= before + 20U);
    CHECK(collector.diagnostics().failed_samples == 0U);
}
