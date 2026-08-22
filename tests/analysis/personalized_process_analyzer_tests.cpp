#include "analysis/personalized_process_analyzer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace analysis = blackbox::analysis;
namespace core = blackbox::core;
using namespace std::chrono_literals;

namespace {

constexpr core::IncidentProcessIdentity compiler_identity{10U, 100U};
constexpr core::IncidentProcessIdentity unrelated_identity{20U, 200U};

[[nodiscard]] std::shared_ptr<const core::IncidentSnapshot> process_spike_incident() {
    core::IncidentHeader header{};
    header.window.event_time = core::MonotonicTimePoint{120s};
    header.window.requested_start = core::MonotonicTimePoint{0s};
    header.window.requested_end = core::MonotonicTimePoint{150s};
    header.actual_start = core::MonotonicTimePoint{0s};
    header.actual_end = core::MonotonicTimePoint{149s};
    std::vector<core::IncidentProcessInfo> metadata(2U);
    metadata[0].identity = compiler_identity;
    metadata[0].name = {"compiler.exe", core::RecordedValueStatus::available};
    metadata[0].executable_path = {"C:\\Tools\\Compiler.exe",
                                   core::RecordedValueStatus::available};
    metadata[1].identity = unrelated_identity;
    metadata[1].name = {"unrelated.exe", core::RecordedValueStatus::available};
    metadata[1].executable_path = {"C:\\Apps\\Unrelated.exe",
                                   core::RecordedValueStatus::available};

    std::vector<core::IncidentSystemSample> systems;
    std::vector<core::IncidentProcessSample> processes;
    systems.reserve(150U);
    processes.reserve(300U);
    for (std::size_t frame = 0U; frame < 150U; ++frame) {
        core::IncidentSystemSample system{};
        system.observed_at = core::MonotonicTimePoint{
            std::chrono::seconds{static_cast<std::int64_t>(frame)}};
        system.cpu_fraction = {0.2, core::RecordedValueStatus::available};
        systems.push_back(system);
        for (const auto identity : {compiler_identity, unrelated_identity}) {
            core::IncidentProcessSample sample{};
            sample.observed_at = system.observed_at;
            sample.identity = identity;
            sample.cpu_fraction = {frame == 120U ? 0.8 : 0.01,
                                   core::RecordedValueStatus::available};
            sample.working_set_bytes = {64U << 20U,
                                        core::RecordedValueStatus::available};
            sample.disk_read_bytes_per_second = {64'000.0,
                                                   core::RecordedValueStatus::available};
            sample.disk_write_bytes_per_second = {32'000.0,
                                                    core::RecordedValueStatus::available};
            processes.push_back(sample);
        }
    }
    return std::make_shared<const core::IncidentSnapshot>(
        std::move(header), std::move(systems), std::move(metadata),
        std::move(processes));
}

[[nodiscard]] const analysis::ProcessAnomaly& process(
    const analysis::IncidentAnalysis& result,
    const core::IncidentProcessIdentity identity) {
    const auto found = std::find_if(result.processes.begin(), result.processes.end(),
                                    [identity](const auto& value) {
                                        return value.identity == identity;
                                    });
    REQUIRE(found != result.processes.end());
    return *found;
}

[[nodiscard]] std::vector<analysis::ExecutableProfileObservation> history(
    const std::int64_t current_time) {
    std::vector<analysis::ExecutableProfileObservation> result;
    for (std::int64_t index = 0; index < 8; ++index) {
        const auto at = current_time - (8 - index) * 1'000;
        result.push_back({"path:c:\\tools\\compiler.exe", "compiler.exe",
                          index + 1, at, 0.8, 64.0 * 1024.0 * 1024.0,
                          64'000.0, 32'000.0});
        result.push_back({"path:c:\\apps\\unrelated.exe", "unrelated.exe",
                          index + 20, at, 0.05, 64.0 * 1024.0 * 1024.0,
                          64'000.0, 32'000.0});
    }
    return result;
}

} // namespace

TEST_CASE("executable identity normalization defines rename upgrade and missing-path policy",
          "[analysis][personalized][identity]") {
    core::IncidentProcessInfo process{};
    process.name = {" Compiler.EXE ", core::RecordedValueStatus::available};
    process.executable_path = {"\\\\?\\C:/Tools//Compiler.EXE\\",
                               core::RecordedValueStatus::available};
    const auto path_identity = analysis::normalize_executable_identity(process);
    REQUIRE(path_identity.has_value());
    CHECK(path_identity->key == "path:c:\\tools\\compiler.exe");
    CHECK(path_identity->source == analysis::ExecutableIdentitySource::normalized_path);

    auto upgraded = process;
    upgraded.name.value = "Compiler 2.0";
    CHECK(analysis::normalize_executable_identity(upgraded)->key == path_identity->key);
    auto renamed = process;
    renamed.executable_path.value = "C:\\Tools\\Renamed.exe";
    CHECK(analysis::normalize_executable_identity(renamed)->key != path_identity->key);

    process.executable_path.status = core::RecordedValueStatus::inaccessible;
    const auto fallback = analysis::normalize_executable_identity(process);
    REQUIRE(fallback.has_value());
    CHECK(fallback->key == "name:compiler.exe");
    CHECK(fallback->source == analysis::ExecutableIdentitySource::normalized_name);
    process.name.status = core::RecordedValueStatus::unsupported;
    CHECK_FALSE(analysis::normalize_executable_identity(process).has_value());
}

TEST_CASE("personalized configuration rejects unbounded or non-aging history",
          "[analysis][personalized][configuration]") {
    CHECK(analysis::validate_personalized_analysis_configuration({}).has_value());
    auto invalid = analysis::PersonalizedAnalysisConfiguration{};
    invalid.maximum_profile_age = 0ms;
    CHECK_FALSE(analysis::validate_personalized_analysis_configuration(invalid).has_value());
    invalid = {};
    invalid.minimum_profile_observations = invalid.maximum_profile_observations + 1U;
    CHECK_FALSE(analysis::validate_personalized_analysis_configuration(invalid).has_value());
}

TEST_CASE("compiler-like CPU becomes normal without normalizing another executable",
          "[analysis][personalized][scoring][acceptance]") {
    constexpr std::int64_t now = 1'800'000'000'000LL;
    const auto fixture = process_spike_incident();
    const auto observations = history(now);
    const analysis::IncidentAnalysisContext context{100, now, observations};
    analysis::PersonalizedProcessAnalyzer analyzer;
    const auto first = analyzer.analyze(*fixture, context);
    const auto second = analyzer.analyze(*fixture, context);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == *second);

    const auto& compiler = process(*first, compiler_identity);
    const auto& unrelated = process(*first, unrelated_identity);
    CHECK(compiler.personalization == analysis::PersonalizationState::ready);
    CHECK(compiler.personalized_observations == 8U);
    CHECK(compiler.score == 0.0);
    CHECK(unrelated.personalization == analysis::PersonalizationState::ready);
    CHECK(unrelated.score > 0.99);
    CHECK(first->processes.front().identity == unrelated_identity);
    CHECK(first->profile_updates.size() == 2U);
}

TEST_CASE("missing and aged executable history remains an explicit profile cold start",
          "[analysis][personalized][cold-start][aging]") {
    constexpr std::int64_t now = 1'800'000'000'000LL;
    const auto fixture = process_spike_incident();
    analysis::PersonalizedProcessAnalyzer analyzer;
    const auto cold = analyzer.analyze(
        *fixture, analysis::IncidentAnalysisContext{100, now, {}});
    REQUIRE(cold.has_value());
    CHECK(process(*cold, compiler_identity).personalization ==
          analysis::PersonalizationState::cold_start);
    CHECK(process(*cold, compiler_identity).confidence == analysis::AnalysisConfidence::low);

    auto aged = history(now);
    for (auto& observation : aged) {
        observation.observed_utc_milliseconds =
            now - std::chrono::duration_cast<std::chrono::milliseconds>(31 * 24h).count();
    }
    const auto expired = analyzer.analyze(
        *fixture, analysis::IncidentAnalysisContext{100, now, aged});
    REQUIRE(expired.has_value());
    CHECK(process(*expired, compiler_identity).personalization ==
          analysis::PersonalizationState::cold_start);
    CHECK(process(*expired, compiler_identity).personalized_observations == 0U);
}
