#include "evaluation/truth_review.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace core = blackbox::core;
namespace evaluation = blackbox::evaluation;
using namespace std::chrono_literals;

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
            ("blackbox-truth-review-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path{};
};

[[nodiscard]] std::string read(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::shared_ptr<const core::IncidentSnapshot> incident() {
    core::IncidentHeader header{};
    header.window.sequence = 8U;
    header.window.event_time = core::MonotonicTimePoint{100s};
    header.window.requested_start = core::MonotonicTimePoint{90s};
    header.window.requested_end = core::MonotonicTimePoint{110s};
    header.window.trigger_count = 1U;
    header.window.automatic_trigger_count = 1U;
    header.window.automatic_resource = core::AutomaticIncidentResource::disk;
    header.window.automatic_signal = core::AutomaticIncidentSignal::disk_latency;
    header.actual_start = core::MonotonicTimePoint{90s};
    header.actual_end = core::MonotonicTimePoint{110s};

    core::IncidentSystemSample before{};
    before.observed_at = core::MonotonicTimePoint{99s};
    before.cpu_fraction = {0.25, core::RecordedValueStatus::available};
    before.memory_fraction = {0.50, core::RecordedValueStatus::available};
    before.disk_read_latency_seconds = {0.125, core::RecordedValueStatus::available};
    before.disk_queue_depth = {4.0, core::RecordedValueStatus::available};
    before.network_tcp_retransmit_fraction.status =
        core::RecordedValueStatus::temporarily_unavailable;
    before.foreground_application = {{918'273U, 192'837U},
                                     core::RecordedValueStatus::available};
    before.memory_pressure_state = {1U, core::RecordedValueStatus::available};
    auto after = before;
    after.observed_at = core::MonotonicTimePoint{101s};
    after.cpu_fraction.value = 0.75;

    const core::IncidentProcessIdentity identity{42U, 123'456U};
    core::IncidentProcessInfo metadata{};
    metadata.identity = identity;
    metadata.name = {"fixture<&>.exe", core::RecordedValueStatus::available};
    metadata.executable_path = {"C:\\Private\\fixture.exe",
                                core::RecordedValueStatus::available};
    core::IncidentProcessSample process{};
    process.observed_at = core::MonotonicTimePoint{99s};
    process.identity = identity;
    process.cpu_fraction = {0.5, core::RecordedValueStatus::available};
    process.working_set_bytes = {64U << 20U, core::RecordedValueStatus::available};

    core::SystemEvent event{};
    event.observed_at = core::MonotonicTimePoint{100s};
    event.source = core::SystemEventSource::application;
    event.kind = core::SystemEventKind::application_crash;
    event.level = core::SystemEventLevel::error;
    event.native_event_id = 1000U;

    core::SystemEvent lifecycle{};
    lifecycle.observed_at = core::MonotonicTimePoint{101s};
    lifecycle.source = core::SystemEventSource::process;
    lifecycle.kind = core::SystemEventKind::process_started;
    lifecycle.has_process_identity = true;
    lifecycle.process_pid = identity.pid;
    lifecycle.process_creation_token = identity.creation_token;

    core::SystemEvent security{};
    security.observed_at = core::MonotonicTimePoint{102s};
    security.source = core::SystemEventSource::security;
    security.kind = core::SystemEventKind::security_scan_started;
    security.level = core::SystemEventLevel::informational;

    return std::make_shared<const core::IncidentSnapshot>(
        std::move(header), std::vector{before, after},
        std::vector{std::move(metadata)}, std::vector{process},
        std::vector{event, lifecycle, security});
}

} // namespace

TEST_CASE("truth review publishes exact prediction-free ordinal evidence atomically") {
    TemporaryDirectory temporary;
    const auto output = temporary.path / "review";
    const auto result = evaluation::export_truth_review(
        *incident(), "00112233445566778899aabbccddeeff", 1'700'000'000'000,
        output);
    REQUIRE(result);
    const evaluation::TruthReviewStatistics expected{2U, 1U, 1U, 3U, false};
    CHECK(*result == expected);
    CHECK_FALSE(std::filesystem::exists(temporary.path / "review.partial"));

    std::set<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator{output}) {
        REQUIRE(entry.is_regular_file());
        REQUIRE(entry.file_size() > 0U);
        files.emplace(entry.path().filename());
    }
    CHECK(files == std::set<std::filesystem::path>{
        "manifest.ini", "system-samples.tsv", "processes.tsv",
        "process-samples.tsv", "system-events.tsv", "ballot-template.tsv",
        "review.html"});

    const auto manifest = read(output / "manifest.ini");
    const auto systems = read(output / "system-samples.tsv");
    const auto processes = read(output / "processes.tsv");
    const auto events = read(output / "system-events.tsv");
    const auto html = read(output / "review.html");
    CHECK(manifest.find("format=1\n") == 0U);
    CHECK(manifest.find("prediction_free=1\n") != std::string::npos);
    CHECK(manifest.find("local_process_identities=0\n") != std::string::npos);
    CHECK(systems.find("memory_pressure_status\tmemory_pressure_state") !=
          std::string::npos);
    CHECK(systems.find("918273") == std::string::npos);
    CHECK(systems.find("192837") == std::string::npos);
    CHECK(processes.find("\n0\t\t\t1\t50") != std::string::npos);
    CHECK(processes.find("fixture") == std::string::npos);
    CHECK(processes.find("123456") == std::string::npos);
    CHECK(events.find("process_started") != std::string::npos);
    CHECK(events.find("\tsecurity\tsecurity_scan_started\t") != std::string::npos);
    CHECK(events.find("\tdefender\t") == std::string::npos);
    CHECK(events.find("\t0\n") != std::string::npos);
    CHECK(events.find("123456") == std::string::npos);
    CHECK(html.find("const sys=[{t:-1,cpu:25") != std::string::npos);
    CHECK(html.find("application_crash") != std::string::npos);
    CHECK(html.find("fixture") == std::string::npos);
    CHECK(html.find("C:\\Private") == std::string::npos);
    CHECK(html.find("diagnosis=") == std::string::npos);
    CHECK(html.find("confidence=") == std::string::npos);
    CHECK(html.find("automatic_trigger") == std::string::npos);
}

TEST_CASE("truth review makes local identities explicit and refuses occupied outputs") {
    TemporaryDirectory temporary;
    const auto output = temporary.path / "local-review";
    auto exported = evaluation::export_truth_review(
        *incident(), "00112233445566778899aabbccddeeff", 0, output,
        {.include_local_process_identities = true});
    REQUIRE(exported);
    CHECK(exported->local_process_identities);
    CHECK(read(output / "manifest.ini").find("local_process_identities=1\n") !=
          std::string::npos);
    const auto processes = read(output / "processes.tsv");
    CHECK(processes.find("\n0\t42\tfixture<&>.exe\t") != std::string::npos);
    const auto html = read(output / "review.html");
    CHECK(html.find("fixture&lt;&amp;&gt;.exe") != std::string::npos);
    CHECK(html.find("C:\\Private") == std::string::npos);
    CHECK(html.find("123456") == std::string::npos);

    const auto repeated = evaluation::export_truth_review(
        *incident(), "00112233445566778899aabbccddeeff", 0, output);
    REQUIRE_FALSE(repeated);
    CHECK(repeated.error().code == evaluation::TruthReviewErrorCode::destination_exists);
    const auto invalid = evaluation::export_truth_review(
        *incident(), "NOT-A-KEY", 0, temporary.path / "invalid");
    REQUIRE_FALSE(invalid);
    CHECK_FALSE(std::filesystem::exists(temporary.path / "invalid"));
    CHECK_FALSE(std::filesystem::exists(temporary.path / "invalid.partial"));
}

TEST_CASE("truth review rejects excessive process identity cardinality before output") {
    core::IncidentHeader header{};
    header.window.event_time = core::MonotonicTimePoint{0s};
    std::vector<core::IncidentProcessSample> samples;
    samples.reserve(evaluation::maximum_truth_review_processes + 1U);
    for (std::size_t index = 0U;
         index <= evaluation::maximum_truth_review_processes; ++index) {
        core::IncidentProcessSample sample{};
        sample.identity = {static_cast<std::uint32_t>(index + 1U), index + 1U};
        samples.push_back(sample);
    }
    core::IncidentSnapshot oversized{std::move(header), {}, {}, std::move(samples)};
    TemporaryDirectory temporary;
    const auto result = evaluation::export_truth_review(
        oversized, "00112233445566778899aabbccddeeff", 0,
        temporary.path / "oversized");
    REQUIRE_FALSE(result);
    CHECK(result.error().code == evaluation::TruthReviewErrorCode::limit_exceeded);
    CHECK_FALSE(std::filesystem::exists(temporary.path / "oversized"));
    CHECK_FALSE(std::filesystem::exists(temporary.path / "oversized.partial"));
}
