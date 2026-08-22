#include "analysis/context_fixture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string_view>
#include <vector>

namespace analysis = blackbox::analysis;
namespace fixture = blackbox::test::context_fixture;

namespace {

constexpr std::array all_contexts{
    analysis::WorkloadContextKind::unknown,
    analysis::WorkloadContextKind::idle,
    analysis::WorkloadContextKind::gaming,
    analysis::WorkloadContextKind::development,
    analysis::WorkloadContextKind::compilation,
    analysis::WorkloadContextKind::video_playback_or_call,
    analysis::WorkloadContextKind::heavy_download,
    analysis::WorkloadContextKind::desktop,
};

[[nodiscard]] constexpr std::string_view name(
    const analysis::WorkloadContextKind context) noexcept {
    switch (context) {
    case analysis::WorkloadContextKind::unknown: return "Unknown";
    case analysis::WorkloadContextKind::idle: return "Idle";
    case analysis::WorkloadContextKind::gaming: return "Gaming";
    case analysis::WorkloadContextKind::development: return "Development";
    case analysis::WorkloadContextKind::compilation: return "Compilation";
    case analysis::WorkloadContextKind::video_playback_or_call: return "Video/call";
    case analysis::WorkloadContextKind::heavy_download: return "Download";
    case analysis::WorkloadContextKind::desktop: return "Desktop";
    }
    return "Unknown";
}

} // namespace

int main() {
    std::array<std::array<std::size_t, all_contexts.size()>, all_contexts.size()> matrix{};
    for (const auto expected : fixture::labeled_contexts) {
        for (std::size_t variant = 0U; variant < 2U; ++variant) {
            const auto assessed = analysis::recognize_workload_context(
                *fixture::incident(expected, variant));
            ++matrix[static_cast<std::size_t>(expected)]
                    [static_cast<std::size_t>(assessed.primary)];
        }
    }

    std::cout << "confusion_matrix_rows_expected_columns_predicted\nexpected";
    for (const auto context : all_contexts) std::cout << ',' << name(context);
    std::cout << '\n';
    for (const auto expected : all_contexts) {
        std::cout << name(expected);
        for (const auto predicted : all_contexts) {
            std::cout << ',' << matrix[static_cast<std::size_t>(expected)]
                                     [static_cast<std::size_t>(predicted)];
        }
        std::cout << '\n';
    }

    constexpr std::size_t trials_per_fixture = 250U;
    std::vector<double> microseconds;
    microseconds.reserve(fixture::labeled_contexts.size() * 2U * trials_per_fixture);
    double checksum{};
    for (const auto context : fixture::labeled_contexts) {
        for (std::size_t variant = 0U; variant < 2U; ++variant) {
            const auto input = fixture::incident(context, variant);
            for (std::size_t trial = 0U; trial < trials_per_fixture; ++trial) {
                const auto started = std::chrono::steady_clock::now();
                const auto assessed = analysis::recognize_workload_context(*input);
                const auto elapsed = std::chrono::steady_clock::now() - started;
                microseconds.push_back(
                    std::chrono::duration<double, std::micro>{elapsed}.count());
                checksum += assessed.confidence;
            }
        }
    }
    std::sort(microseconds.begin(), microseconds.end());
    const auto percentile = [&microseconds](const std::size_t numerator) {
        const auto rank = (microseconds.size() * numerator + 99U) / 100U;
        return microseconds[(std::max<std::size_t>)(1U, rank) - 1U];
    };
    const auto average = std::accumulate(microseconds.begin(), microseconds.end(), 0.0) /
                         static_cast<double>(microseconds.size());
    std::cout << std::fixed << std::setprecision(3)
              << "recognition_runs," << microseconds.size() << '\n'
              << "average_us," << average << '\n'
              << "p95_us," << percentile(95U) << '\n'
              << "p99_us," << percentile(99U) << '\n'
              << "maximum_us," << microseconds.back() << '\n'
              << "checksum," << checksum << '\n';

    std::size_t baseline_correct{};
    std::size_t contextual_correct{};
    std::size_t protected_regressions{};
    std::size_t scenarios{};
    for (const auto context : fixture::labeled_contexts) {
        if (context == analysis::WorkloadContextKind::idle ||
            context == analysis::WorkloadContextKind::unknown) continue;
        ++scenarios;
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
    std::cout << "held_out_scenarios," << scenarios << '\n'
              << "baseline_top1_correct," << baseline_correct << '\n'
              << "contextual_top1_correct," << contextual_correct << '\n'
              << "protected_baseline_regressions," << protected_regressions << '\n';
    return contextual_correct > baseline_correct && protected_regressions == 0U ? 0 : 1;
}
