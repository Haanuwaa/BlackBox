#include "evaluation/campaign_status.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

namespace evaluation = blackbox::evaluation;

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
            ("blackbox-campaign-status-" + std::to_string(
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

} // namespace

TEST_CASE("campaign status publishes exact prediction-free v1 output atomically",
          "[evaluation][dogfood][campaign]") {
    TemporaryDirectory temporary;
    const auto corpus_path = temporary.path / "corpus";
    REQUIRE(evaluation::initialize_dogfood_corpus(
        corpus_path, "campaign-v1", 12U, 16'382'915'624'291'673'744ULL));
    const auto corpus = evaluation::load_dogfood_corpus(corpus_path);
    REQUIRE(corpus);

    const auto output = temporary.path / "status";
    const auto result = evaluation::export_campaign_status(*corpus, 1'700'000'000'000, output);
    REQUIRE(result);
    const evaluation::CampaignStatusStatistics expected{0U, 9U, 7U, false};
    CHECK(*result == expected);
    CHECK_FALSE(std::filesystem::exists(temporary.path / "status.partial"));

    std::set<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator{output}) {
        REQUIRE(entry.is_regular_file());
        REQUIRE(entry.file_size() > 0U);
        files.emplace(entry.path().filename());
    }
    CHECK(files == std::set<std::filesystem::path>{
        "manifest.ini", "summary.tsv", "profiles.tsv", "symptoms.tsv",
        "unmet.tsv", "status.html"});

    const auto manifest = read(output / "manifest.ini");
    const auto summary = read(output / "summary.tsv");
    const auto symptoms = read(output / "symptoms.tsv");
    const auto html = read(output / "status.html");
    CHECK(manifest.find("format=1\n") == 0U);
    CHECK(manifest.find("prediction_free=1\n") != std::string::npos);
    CHECK(manifest.find("evidence_neutral=1\n") != std::string::npos);
    CHECK(manifest.find("qualification_ready=0\n") != std::string::npos);
    CHECK(summary.find("natural_sessions\t0\t6\t0\n") != std::string::npos);
    CHECK(symptoms.find("cpu_starvation\t0\t0\n") != std::string::npos);
    CHECK(symptoms.find("application_crash\t0\t0\n") != std::string::npos);
    CHECK(html.find("Prediction-free, evidence-neutral status") != std::string::npos);
    CHECK(html.find("not ready") != std::string::npos);
    CHECK(html.find("diagnosis=") == std::string::npos);
    CHECK(html.find("confidence=") == std::string::npos);
    CHECK(html.find("analyzer") == std::string::npos);
}

TEST_CASE("campaign status refuses occupied outputs and invalid corpus",
          "[evaluation][dogfood][campaign]") {
    TemporaryDirectory temporary;
    const auto corpus_path = temporary.path / "corpus";
    REQUIRE(evaluation::initialize_dogfood_corpus(corpus_path, "campaign-v1", 12U, 99U));
    auto corpus = evaluation::load_dogfood_corpus(corpus_path);
    REQUIRE(corpus);
    const auto output = temporary.path / "status";
    REQUIRE(evaluation::export_campaign_status(*corpus, 0, output));

    const auto occupied = evaluation::export_campaign_status(*corpus, 0, output);
    REQUIRE_FALSE(occupied);
    CHECK(occupied.error().code == evaluation::CampaignStatusErrorCode::destination_exists);

    corpus->manifest.corpus_id.clear();
    const auto invalid = evaluation::export_campaign_status(
        *corpus, 0, temporary.path / "invalid-status");
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == evaluation::CampaignStatusErrorCode::corpus_invalid);
    CHECK_FALSE(std::filesystem::exists(temporary.path / "invalid-status"));
    CHECK_FALSE(std::filesystem::exists(temporary.path / "invalid-status.partial"));
}
