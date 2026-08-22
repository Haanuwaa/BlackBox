#include "analysis/contributor_ranker.hpp"
#include "analysis/personalized_process_analyzer.hpp"
#include "analysis/statistical_incident_analyzer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>

namespace analysis = blackbox::analysis;
namespace core = blackbox::core;
using namespace std::chrono_literals;

namespace {

enum class ContributorMetric { cpu, memory, disk };

[[nodiscard]] const analysis::ContributorCandidate& candidate_for(
    const analysis::IncidentAnalysis& result,
    core::IncidentProcessIdentity identity);

[[nodiscard]] std::shared_ptr<const core::IncidentSnapshot> contributor_fixture(
    const ContributorMetric metric, const bool intended_missing_metrics = false,
    const bool intended_short_lived = false) {
    constexpr std::size_t frame_count = 150U;
    core::IncidentHeader header{};
    header.window.event_time = core::MonotonicTimePoint{120s};
    header.window.requested_start = core::MonotonicTimePoint{0s};
    header.window.requested_end = core::MonotonicTimePoint{149s};
    header.actual_start = header.window.requested_start;
    header.actual_end = header.window.requested_end;

    constexpr core::IncidentProcessIdentity normal{10U, 100U};
    constexpr core::IncidentProcessIdentity intended{20U, 200U};
    constexpr core::IncidentProcessIdentity follower{30U, 300U};
    constexpr core::IncidentProcessIdentity ambiguous{40U, 400U};
    std::vector<core::IncidentProcessInfo> metadata;
    for (const auto& [identity, name] : {
             std::pair{normal, "normal.exe"}, std::pair{intended, "intended.exe"},
             std::pair{follower, "follower.exe"},
             std::pair{ambiguous, "ambiguous.exe"}}) {
        core::IncidentProcessInfo info{};
        info.identity = identity;
        info.name = {name, core::RecordedValueStatus::available};
        metadata.push_back(std::move(info));
    }

    std::vector<core::IncidentSystemSample> systems;
    std::vector<core::IncidentProcessSample> processes;
    systems.reserve(frame_count);
    processes.reserve(frame_count * metadata.size());
    for (std::size_t frame = 0U; frame < frame_count; ++frame) {
        const auto observed_at = core::MonotonicTimePoint{
            std::chrono::seconds{static_cast<std::int64_t>(frame)}};
        const auto wave = static_cast<double>(static_cast<int>(frame % 5U) - 2);
        const auto system_event = frame >= 112U && frame <= 133U;
        core::IncidentSystemSample system{};
        system.observed_at = observed_at;
        system.cpu_fraction = {
            metric == ContributorMetric::cpu && system_event ? 0.95
                                                              : 0.20 + wave * 0.002,
            core::RecordedValueStatus::available};
        system.memory_fraction = {0.50 + wave * 0.001,
                                  core::RecordedValueStatus::available};
        if (metric == ContributorMetric::memory && system_event)
            system.memory_fraction.value = 0.92;
        system.disk_read_bytes_per_second = {
            metric == ContributorMetric::disk && system_event
                ? 200.0 * 1024.0 * 1024.0
                : 1'048'576.0 + wave * 4'096.0,
            core::RecordedValueStatus::available};
        system.disk_write_bytes_per_second = {524'288.0,
                                               core::RecordedValueStatus::available};
        system.network_receive_bytes_per_second = {262'144.0,
                                                    core::RecordedValueStatus::available};
        system.network_transmit_bytes_per_second = {131'072.0,
                                                     core::RecordedValueStatus::available};
        systems.push_back(system);

        for (const auto& info : metadata) {
            if (intended_short_lived && info.identity == intended && frame < 112U) continue;
            const auto preceding = info.identity == intended &&
                                   frame >= 112U && frame <= 121U;
            const auto reacting = info.identity == follower &&
                                  frame >= 124U && frame <= 133U;
            const auto spanning = info.identity == ambiguous &&
                                  frame >= 119U && frame <= 133U;
            const auto event = preceding || reacting || spanning;
            core::IncidentProcessSample process{};
            process.observed_at = observed_at;
            process.identity = info.identity;
            process.cpu_fraction = {
                metric == ContributorMetric::cpu && event ? 0.80
                    : 0.02 + wave * 0.0001,
                core::RecordedValueStatus::available};
            process.working_set_bytes = {
                metric == ContributorMetric::memory && event
                    ? static_cast<std::uint64_t>(900U) << 20U
                    : static_cast<std::uint64_t>(96U) << 20U,
                                         core::RecordedValueStatus::available};
            process.disk_read_bytes_per_second = {
                metric == ContributorMetric::disk && event
                    ? 150.0 * 1024.0 * 1024.0
                    : 64'000.0 + wave * 100.0,
                core::RecordedValueStatus::available};
            process.disk_write_bytes_per_second = {32'000.0,
                                                    core::RecordedValueStatus::available};
            if (intended_missing_metrics && info.identity == intended) {
                process.working_set_bytes.status = core::RecordedValueStatus::unsupported;
                process.disk_read_bytes_per_second.status =
                    metric == ContributorMetric::disk
                        ? core::RecordedValueStatus::available
                        : core::RecordedValueStatus::unsupported;
                process.disk_write_bytes_per_second.status =
                    core::RecordedValueStatus::unsupported;
                if (metric == ContributorMetric::disk)
                    process.cpu_fraction.status = core::RecordedValueStatus::unsupported;
            }
            processes.push_back(process);
        }
    }
    return std::make_shared<const core::IncidentSnapshot>(
        std::move(header), std::move(systems), std::move(metadata),
        std::move(processes));
}

TEST_CASE("short-lived high-activity process remains a low-confidence potential contributor",
          "[analysis][contributor][cold-start]") {
    analysis::StatisticalIncidentAnalyzer analyzer;
    const auto result = analyzer.analyze(
        *contributor_fixture(ContributorMetric::cpu, false, true));
    REQUIRE(result.has_value());
    const auto& candidate = candidate_for(
        *result, core::IncidentProcessIdentity{20U, 200U});
    CHECK(candidate.confidence == analysis::AnalysisConfidence::low);
    CHECK(candidate.strength == analysis::ContributorStrength::potential);
    CHECK(candidate.temporal_relationship ==
          analysis::ContributorTemporalRelationship::preceding_activity);
    CHECK(candidate.anomaly_magnitude > 0.99);
    const auto ranked = std::find_if(result->contributors.begin(),
                                     result->contributors.end(),
                                     [](const auto& value) {
                                         return value.identity ==
                                             core::IncidentProcessIdentity{20U, 200U};
                                     });
    CHECK(std::distance(result->contributors.begin(), ranked) < 3);
}

[[nodiscard]] const analysis::ContributorCandidate& candidate_for(
    const analysis::IncidentAnalysis& result,
    const core::IncidentProcessIdentity identity) {
    const auto found = std::find_if(result.contributors.begin(), result.contributors.end(),
                                    [identity](const auto& value) {
                                        return value.identity == identity;
                                    });
    REQUIRE(found != result.contributors.end());
    return *found;
}

} // namespace

TEST_CASE("preceding process activity outranks unrelated post-marker reactions",
          "[analysis][contributor][timing][quality]") {
    analysis::StatisticalIncidentAnalyzer analyzer;
    for (const auto metric : {ContributorMetric::cpu, ContributorMetric::memory,
                              ContributorMetric::disk}) {
        const auto result = analyzer.analyze(*contributor_fixture(metric));
        REQUIRE(result.has_value());
        REQUIRE(result->contributors.size() >= 2U);
        CHECK(result->contributors.front().identity ==
              core::IncidentProcessIdentity{20U, 200U});
        const auto& preceding = candidate_for(
            *result, core::IncidentProcessIdentity{20U, 200U});
        const auto& follower = candidate_for(
            *result, core::IncidentProcessIdentity{30U, 300U});
        const auto& ambiguous = candidate_for(
            *result, core::IncidentProcessIdentity{40U, 400U});
        CHECK(preceding.temporal_relationship ==
              analysis::ContributorTemporalRelationship::preceding_activity);
        CHECK(follower.temporal_relationship ==
              analysis::ContributorTemporalRelationship::post_marker_reaction);
        CHECK(ambiguous.temporal_relationship ==
              analysis::ContributorTemporalRelationship::marker_spanning_ambiguous);
        CHECK(preceding.pre_marker_anomalous_samples == 9U);
        CHECK(preceding.post_marker_anomalous_samples == 1U);
        CHECK(ambiguous.pre_marker_anomalous_samples == 2U);
        CHECK(ambiguous.post_marker_anomalous_samples == 13U);
        CHECK(follower.pre_marker_anomalous_samples == 0U);
        CHECK(follower.post_marker_anomalous_samples == 10U);
        CHECK(preceding.score > follower.score);
        CHECK(preceding.anomaly_magnitude > 0.99);
        CHECK(preceding.resource_match_score > 0.99);
        CHECK(preceding.timing_score == 1.0);
        CHECK(preceding.duration_score == 1.0);
        CHECK(preceding.strength == analysis::ContributorStrength::likely);
        CHECK(follower.strength == analysis::ContributorStrength::potential);
        CHECK(ambiguous.strength == analysis::ContributorStrength::potential);
        CHECK(ambiguous.timing_score == 0.35);
    }
}

TEST_CASE("contributor context distinguishes exact process lifecycle from activity timing",
          "[analysis][contributor][timing][process-lifecycle][privacy]") {
    analysis::StatisticalIncidentAnalyzer analyzer;
    const auto base = contributor_fixture(ContributorMetric::cpu);
    const auto baseline = analyzer.analyze(*base);
    REQUIRE(baseline.has_value());
    const auto baseline_score = candidate_for(
        *baseline, core::IncidentProcessIdentity{20U, 200U}).score;

    const auto lifecycle_event = [](const core::MonotonicTimePoint observed_at,
                                    const core::SystemEventKind kind,
                                    const core::IncidentProcessIdentity identity) {
        core::SystemEvent event{};
        event.observed_at = observed_at;
        event.source = core::SystemEventSource::process;
        event.kind = kind;
        event.has_process_identity = true;
        event.process_pid = identity.pid;
        event.process_creation_token = identity.creation_token;
        return event;
    };
    std::vector<core::SystemEvent> events;
    events.push_back(lifecycle_event(
        core::MonotonicTimePoint{111s}, core::SystemEventKind::process_started,
        {20U, 200U}));
    events.push_back(lifecycle_event(
        core::MonotonicTimePoint{122s}, core::SystemEventKind::process_exited,
        {20U, 200U}));
    // A reused PID and a start observation after anomalous activity began are
    // not allowed to attach to this candidate.
    events.push_back(lifecycle_event(
        core::MonotonicTimePoint{100s}, core::SystemEventKind::process_started,
        {20U, 201U}));
    events.push_back(lifecycle_event(
        core::MonotonicTimePoint{119s}, core::SystemEventKind::process_started,
        {20U, 200U}));
    events.push_back(lifecycle_event(
        core::MonotonicTimePoint{-1s}, core::SystemEventKind::process_started,
        {20U, 200U}));
    auto wrong_source = lifecycle_event(
        core::MonotonicTimePoint{110s}, core::SystemEventKind::process_started,
        {20U, 200U});
    wrong_source.source = core::SystemEventSource::application;
    events.push_back(wrong_source);

    const core::IncidentSnapshot incident{
        base->header(),
        std::vector<core::IncidentSystemSample>{base->system_samples().begin(),
                                                base->system_samples().end()},
        std::vector<core::IncidentProcessInfo>{base->process_metadata().begin(),
                                               base->process_metadata().end()},
        std::vector<core::IncidentProcessSample>{base->process_samples().begin(),
                                                 base->process_samples().end()},
        std::move(events)};
    const auto result = analyzer.analyze(incident);
    REQUIRE(result.has_value());
    const auto& candidate = candidate_for(
        *result, core::IncidentProcessIdentity{20U, 200U});
    CHECK(candidate.activity_started_seconds_from_event == -8.0);
    CHECK(candidate.has_process_start_event);
    CHECK(candidate.process_started_seconds_from_event == -9.0);
    CHECK(candidate.has_process_exit_event);
    CHECK(candidate.process_exited_seconds_from_event == 2.0);
    CHECK(candidate.score == baseline_score);

    const auto& follower = candidate_for(
        *result, core::IncidentProcessIdentity{30U, 300U});
    CHECK_FALSE(follower.has_process_start_event);
    CHECK_FALSE(follower.has_process_exit_event);
}

TEST_CASE("missing process metrics reduce contributor coverage and confidence",
          "[analysis][contributor][missing]") {
    analysis::StatisticalIncidentAnalyzer analyzer;
    const auto complete = analyzer.analyze(
        *contributor_fixture(ContributorMetric::cpu, false));
    const auto missing = analyzer.analyze(
        *contributor_fixture(ContributorMetric::cpu, true));
    REQUIRE(complete.has_value());
    REQUIRE(missing.has_value());
    const auto& complete_candidate = candidate_for(
        *complete, core::IncidentProcessIdentity{20U, 200U});
    const auto& missing_candidate = candidate_for(
        *missing, core::IncidentProcessIdentity{20U, 200U});
    CHECK(missing_candidate.missing_metrics == 3U);
    CHECK(missing_candidate.evidence_coverage < complete_candidate.evidence_coverage);
    CHECK(missing_candidate.score < complete_candidate.score);
    CHECK(missing_candidate.confidence == analysis::AnalysisConfidence::low);
    CHECK(missing_candidate.strength == analysis::ContributorStrength::potential);
}

TEST_CASE("resource match requires aligned anomaly direction",
          "[analysis][contributor][resource]") {
    analysis::StatisticalIncidentAnalyzer analyzer;
    const auto incident = contributor_fixture(ContributorMetric::cpu);
    auto analyzed = analyzer.analyze(*incident);
    REQUIRE(analyzed.has_value());
    const auto resource = std::find_if(
        analyzed->resources.begin(), analyzed->resources.end(), [](const auto& value) {
            return value.resource == analysis::ResourceKind::cpu;
        });
    REQUIRE(resource != analyzed->resources.end());
    for (auto& evidence : resource->evidence) {
        if (evidence.direction == analysis::AnomalyDirection::higher)
            evidence.direction = analysis::AnomalyDirection::lower;
    }

    analyzed->contributors = analysis::rank_contributors(*incident, *analyzed);
    const auto& candidate = candidate_for(
        *analyzed, core::IncidentProcessIdentity{20U, 200U});
    CHECK(candidate.resource_match_score == 0.0);
    CHECK(candidate.strength == analysis::ContributorStrength::potential);
}

TEST_CASE("recurring matching activity contributes bounded inspectable evidence",
          "[analysis][contributor][recurrence]") {
    analysis::PersonalizedProcessAnalyzer analyzer;
    const auto fixture = contributor_fixture(ContributorMetric::cpu);
    std::vector<analysis::ExecutableProfileObservation> history;
    for (std::int64_t id = 1; id <= 3; ++id) {
        history.push_back(analysis::ExecutableProfileObservation{
            "name:intended.exe", "intended.exe", id, 1'000 + id,
            0.80, std::nullopt, std::nullopt, std::nullopt});
    }
    const analysis::IncidentAnalysisContext context{10, 2'000, history};
    const auto result = analyzer.analyze(*fixture, context);
    REQUIRE(result.has_value());
    const auto& candidate = candidate_for(
        *result, core::IncidentProcessIdentity{20U, 200U});
    CHECK(candidate.recurrence_count == 3U);
    CHECK(candidate.recurrence_score == 1.0);
    CHECK(result->contributors.size() <= analysis::maximum_contributor_candidates);
}
