#include "context_fixture.hpp"
#include "analysis/statistical_incident_analyzer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numeric>

namespace analysis = blackbox::analysis;
namespace fixture = blackbox::test::context_fixture;

TEST_CASE("workload context configuration enforces hard bounds",
          "[analysis][context][configuration]") {
    CHECK(analysis::validate_workload_context_configuration({}).has_value());
    auto invalid = analysis::WorkloadContextConfiguration{};
    invalid.maximum_process_metadata = analysis::maximum_context_process_metadata + 1U;
    CHECK_FALSE(analysis::validate_workload_context_configuration(invalid).has_value());
    invalid = {};
    invalid.maximum_evidence = analysis::maximum_context_evidence + 1U;
    CHECK_FALSE(analysis::validate_workload_context_configuration(invalid).has_value());
    invalid = {};
    invalid.maximum_score_reduction = 0.251;
    CHECK_FALSE(analysis::validate_workload_context_configuration(invalid).has_value());
}

TEST_CASE("labeled workload fixtures produce deterministic probability distributions",
          "[analysis][context][confusion]") {
    for (const auto expected : fixture::labeled_contexts) {
        for (std::size_t variant = 0U; variant < 2U; ++variant) {
            const auto input = fixture::incident(expected, variant);
            const auto first = analysis::recognize_workload_context(*input);
            const auto second = analysis::recognize_workload_context(*input);
            CHECK(first == second);
            CHECK(first.enabled);
            CHECK(first.primary == expected);
            REQUIRE(first.probabilities.size() == fixture::labeled_contexts.size());
            const auto sum = std::accumulate(
                first.probabilities.begin(), first.probabilities.end(), 0.0,
                [](const double total, const auto& value) {
                    return total + value.probability;
                });
            CHECK(sum == Catch::Approx(1.0).margin(1e-12));
            CHECK(first.confidence > 0.50);
            CHECK(first.uncertainty >= 0.0);
            CHECK(first.uncertainty <= 1.0);
            CHECK(first.evidence.size() <= analysis::maximum_context_evidence);
        }
    }
}

TEST_CASE("ambiguous workloads explicitly retain Unknown and high uncertainty",
          "[analysis][context][unknown]") {
    const auto assessed = analysis::recognize_workload_context(
        *fixture::incident(analysis::WorkloadContextKind::unknown));
    CHECK(assessed.primary == analysis::WorkloadContextKind::unknown);
    CHECK(assessed.uncertainty > 0.50);
    REQUIRE_FALSE(assessed.evidence.empty());
    CHECK(assessed.evidence.front().signal == analysis::ContextSignalKind::ambiguous_margin);
}

TEST_CASE("missing telemetry is uncertainty rather than an Idle assumption",
          "[analysis][context][missing]") {
    const blackbox::core::IncidentSnapshot empty{
        {}, {}, {}, {}};
    const auto assessed = analysis::recognize_workload_context(empty);
    CHECK(assessed.primary == analysis::WorkloadContextKind::unknown);
    CHECK(assessed.uncertainty > 0.90);
}

TEST_CASE("context can be disabled independently and leaves analyzer ranking unadjusted",
          "[analysis][context][optional]") {
    analysis::StatisticalAnalysisConfiguration configuration{};
    configuration.workload_context.enabled = false;
    analysis::StatisticalIncidentAnalyzer analyzer{configuration};
    const auto result = analyzer.analyze(
        *fixture::incident(analysis::WorkloadContextKind::compilation));
    REQUIRE(result.has_value());
    CHECK_FALSE(result->workload_context.enabled);
    REQUIRE(result->workload_context.probabilities.size() == 1U);
    CHECK(result->workload_context.probabilities.front().context ==
          analysis::WorkloadContextKind::unknown);
    for (const auto& resource : result->resources) {
        CHECK(resource.context_multiplier == 1.0);
        CHECK(resource.score == resource.uncontextualized_score);
    }
}

TEST_CASE("probabilities softly rerank expected resources while preserving raw evidence",
          "[analysis][context][ranking]") {
    const auto context = analysis::WorkloadContextKind::compilation;
    const auto assessment = analysis::recognize_workload_context(
        *fixture::incident(context, 1U));
    auto ranking = fixture::ranking_fixture(assessment, context);
    const auto before = ranking.resources.front().resource;
    analysis::apply_workload_context(ranking, {});
    CHECK(before == fixture::expected_resource(context));
    CHECK(ranking.resources.front().resource == fixture::held_out_target(context));
    for (const auto& resource : ranking.resources) {
        CHECK(resource.uncontextualized_score >= resource.score);
        CHECK(resource.context_multiplier >= 0.75);
        CHECK(resource.context_multiplier <= 1.0);
    }
}

TEST_CASE("held-out context ranking gains top-one accuracy without regressions",
          "[analysis][context][quality][held-out]") {
    std::size_t baseline_correct{};
    std::size_t contextual_correct{};
    std::size_t protected_regressions{};
    for (const auto context : fixture::labeled_contexts) {
        if (context == analysis::WorkloadContextKind::idle ||
            context == analysis::WorkloadContextKind::unknown) continue;
        const auto assessment = analysis::recognize_workload_context(
            *fixture::incident(context, 1U));
        auto ranking = fixture::ranking_fixture(assessment, context);
        baseline_correct += ranking.resources.front().resource ==
                            fixture::held_out_target(context);
        analysis::apply_workload_context(ranking, {});
        contextual_correct += ranking.resources.front().resource ==
                              fixture::held_out_target(context);

        auto protected_ranking = fixture::ranking_fixture(assessment, context, true);
        analysis::apply_workload_context(protected_ranking, {});
        protected_regressions += protected_ranking.resources.front().resource !=
                                 fixture::held_out_target(context);
    }
    CHECK(contextual_correct > baseline_correct);
    CHECK(contextual_correct == 6U);
    CHECK(protected_regressions == 0U);
}
