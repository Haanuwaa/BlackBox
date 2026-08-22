#include "evaluation/evaluation_run_transaction.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <latch>
#include <string>
#include <thread>

namespace evaluation = blackbox::evaluation;

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
            ("blackbox-evaluation-transaction-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path{};
};

void write_file(const std::filesystem::path& path, const std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    REQUIRE(output.good());
}

} // namespace

TEST_CASE("evaluation output publishes only a complete sibling staging directory",
          "[evaluation][transaction][atomic]") {
    TemporaryDirectory temporary;
    const auto output = temporary.path / "calibration-result";
    REQUIRE(evaluation::validate_evaluation_output_destination(output));
    auto transaction = evaluation::begin_evaluation_output(output);
    REQUIRE(transaction.has_value());
    CHECK_FALSE(std::filesystem::exists(output));
    CHECK(std::filesystem::is_directory(transaction->staging_directory));

    write_file(transaction->staging_directory / "evaluation.json", "{}\n");
    write_file(transaction->staging_directory / "predictions.tsv", "header\n");
    constexpr std::array required_incomplete{
        std::string_view{"evaluation.json"}, std::string_view{"predictions.tsv"},
        std::string_view{"calibration.tsv"}};
    auto published = evaluation::publish_evaluation_output(
        *transaction, required_incomplete);
    REQUIRE_FALSE(published.has_value());
    CHECK(published.error().code ==
          evaluation::EvaluationArtifactErrorCode::incomplete);
    CHECK_FALSE(std::filesystem::exists(output));
    CHECK(std::filesystem::is_directory(transaction->staging_directory));

    write_file(transaction->staging_directory / "calibration.tsv", "model\n");
    write_file(transaction->staging_directory / "unexpected.tmp", "junk\n");
    published = evaluation::publish_evaluation_output(*transaction, required_incomplete);
    REQUIRE_FALSE(published.has_value());
    CHECK(published.error().code ==
          evaluation::EvaluationArtifactErrorCode::incomplete);
    REQUIRE(std::filesystem::remove(
        transaction->staging_directory / "unexpected.tmp"));
    REQUIRE(evaluation::publish_evaluation_output(*transaction, required_incomplete));
    CHECK(std::filesystem::is_directory(output));
    CHECK_FALSE(std::filesystem::exists(transaction->staging_directory));
    CHECK(std::filesystem::is_regular_file(output / "calibration.tsv"));

    auto repeated = evaluation::begin_evaluation_output(output);
    REQUIRE_FALSE(repeated.has_value());
    CHECK(repeated.error().code ==
          evaluation::EvaluationArtifactErrorCode::already_exists);
}

TEST_CASE("artifact fingerprints are bounded content identities independent of input order",
          "[evaluation][transaction][fingerprint]") {
    TemporaryDirectory temporary;
    const auto first = temporary.path / "evaluation.json";
    const auto second = temporary.path / "predictions.tsv";
    write_file(first, "report-a\n");
    write_file(second, "rows-a\n");
    const std::array forward{first, second};
    const std::array reverse{second, first};
    auto first_hash = evaluation::evaluation_artifact_fingerprint(forward);
    auto reverse_hash = evaluation::evaluation_artifact_fingerprint(reverse);
    REQUIRE(first_hash.has_value());
    REQUIRE(reverse_hash.has_value());
    CHECK(*first_hash == *reverse_hash);

    write_file(second, "rows-b\n");
    auto changed = evaluation::evaluation_artifact_fingerprint(forward);
    REQUIRE(changed.has_value());
    CHECK(*changed != *first_hash);

    const auto empty = temporary.path / "empty.tsv";
    write_file(empty, "");
    const std::array empty_set{empty};
    auto rejected = evaluation::evaluation_artifact_fingerprint(empty_set);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code ==
          evaluation::EvaluationArtifactErrorCode::incomplete);
}

TEST_CASE("held-out attempt is exclusively acquired before work and preserves crash state",
          "[evaluation][transaction][held-out][race]") {
    TemporaryDirectory temporary;
    auto initial = evaluation::held_out_evaluation_status(temporary.path);
    REQUIRE(initial.has_value());
    CHECK(initial->state == evaluation::HeldOutEvaluationState::not_started);

    std::array<bool, 2U> acquired{};
    std::latch start{2};
    std::jthread first([&] {
        start.arrive_and_wait();
        acquired[0U] = evaluation::acquire_held_out_evaluation_attempt(
            temporary.path, 11U, 22U, 33U).has_value();
    });
    std::jthread second([&] {
        start.arrive_and_wait();
        acquired[1U] = evaluation::acquire_held_out_evaluation_attempt(
            temporary.path, 11U, 22U, 33U).has_value();
    });
    first.join();
    second.join();
    CHECK(acquired[0U] != acquired[1U]);

    auto running = evaluation::held_out_evaluation_status(temporary.path);
    REQUIRE(running.has_value());
    CHECK(running->state == evaluation::HeldOutEvaluationState::running);
    CHECK(running->annotation_fingerprint == 11U);
    CHECK(running->configuration_fingerprint == 22U);
    CHECK(running->calibration_artifact_fingerprint == 33U);
    CHECK_FALSE(running->qualification_passed.has_value());

    auto repeated = evaluation::acquire_held_out_evaluation_attempt(
        temporary.path, 11U, 22U, 33U);
    REQUIRE_FALSE(repeated.has_value());
    CHECK(repeated.error().code ==
          evaluation::EvaluationArtifactErrorCode::already_exists);

    const evaluation::HeldOutEvaluationAttempt attempt{
        temporary.path / "heldout-evaluation.lock", 11U, 22U, 33U};
    REQUIRE(evaluation::complete_held_out_evaluation_attempt(
        attempt, false, 44U));
    auto complete = evaluation::held_out_evaluation_status(temporary.path);
    REQUIRE(complete.has_value());
    CHECK(complete->state == evaluation::HeldOutEvaluationState::complete);
    REQUIRE(complete->qualification_passed.has_value());
    CHECK_FALSE(*complete->qualification_passed);
    REQUIRE(complete->report_artifact_fingerprint.has_value());
    CHECK(*complete->report_artifact_fingerprint == 44U);

    auto duplicate_completion = evaluation::complete_held_out_evaluation_attempt(
        attempt, true, 55U);
    REQUIRE_FALSE(duplicate_completion.has_value());
    CHECK(duplicate_completion.error().code ==
          evaluation::EvaluationArtifactErrorCode::invalid_state);
}
