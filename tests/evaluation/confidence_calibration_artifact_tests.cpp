#include "evaluation/confidence_calibration_artifact.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace evaluation = blackbox::evaluation;

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
            ("blackbox-calibration-artifact-" + std::to_string(
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
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

void replace_once(std::string& text, const std::string_view needle,
                  const std::string_view replacement) {
    const auto offset = text.find(needle);
    REQUIRE(offset != std::string::npos);
    text.replace(offset, needle.size(), replacement);
}

[[nodiscard]] evaluation::ConfidenceCalibrationArtifact artifact() {
    return {
        123'456U,
        789'012U,
        {10U, {{0.2, 0.1, 4U}, {0.8, 0.75, 6U}}},
        {true, 0.75, 2.0 / 3.0, 10U, 5U, 4.0 / 5.0},
    };
}

} // namespace

TEST_CASE("confidence calibration artifacts enforce one bounded canonical V1",
          "[evaluation][calibration][artifact][format]") {
    TemporaryDirectory temporary;
    const auto expected = artifact();
    const auto serialized =
        evaluation::serialize_confidence_calibration_artifact(expected);
    REQUIRE(serialized.has_value());
    CHECK(serialized->contains(
        "assertion_threshold=0.66666666666666663\n"));
    CHECK(serialized->contains(
        "observed_assertion_precision=0.80000000000000004\n"));
    CHECK(serialized->back() == '\n');

    const auto canonical_path = temporary.path / "calibration.tsv";
    REQUIRE(evaluation::write_confidence_calibration_artifact(
        canonical_path, expected));
    CHECK_FALSE(std::filesystem::exists(
        temporary.path / "calibration.tsv.partial"));
    const auto loaded =
        evaluation::load_confidence_calibration_artifact(canonical_path);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == expected);

    const auto repeated = evaluation::write_confidence_calibration_artifact(
        canonical_path, expected);
    REQUIRE_FALSE(repeated.has_value());
    CHECK(repeated.error().code ==
          evaluation::ConfidenceCalibrationArtifactErrorCode::already_exists);

    auto noncanonical = *serialized;
    replace_once(noncanonical, "minimum_assertion_precision=0.75\n",
                 "minimum_assertion_precision=0.750\n");
    const auto noncanonical_path = temporary.path / "noncanonical.tsv";
    write_file(noncanonical_path, noncanonical);
    const auto rejected_noncanonical =
        evaluation::load_confidence_calibration_artifact(noncanonical_path);
    REQUIRE_FALSE(rejected_noncanonical.has_value());
    CHECK(rejected_noncanonical.error().code ==
          evaluation::ConfidenceCalibrationArtifactErrorCode::noncanonical);

    auto crlf = *serialized;
    replace_once(crlf, "format=blackbox-confidence-calibration\n",
                 "format=blackbox-confidence-calibration\r\n");
    const auto crlf_path = temporary.path / "crlf.tsv";
    write_file(crlf_path, crlf);
    const auto rejected_crlf =
        evaluation::load_confidence_calibration_artifact(crlf_path);
    REQUIRE_FALSE(rejected_crlf.has_value());
    CHECK(rejected_crlf.error().code ==
          evaluation::ConfidenceCalibrationArtifactErrorCode::invalid_format);

    auto reordered = *serialized;
    replace_once(reordered,
                 "annotation_fingerprint=123456\n"
                 "configuration_fingerprint=789012\n",
                 "configuration_fingerprint=789012\n"
                 "annotation_fingerprint=123456\n");
    const auto reordered_path = temporary.path / "reordered.tsv";
    write_file(reordered_path, reordered);
    CHECK_FALSE(evaluation::load_confidence_calibration_artifact(
        reordered_path));

    auto wrong_count = *serialized;
    replace_once(wrong_count, "0.80000000000000004\t0.75\t6\n",
                 "0.80000000000000004\t0.75\t5\n");
    const auto wrong_count_path = temporary.path / "wrong-count.tsv";
    write_file(wrong_count_path, wrong_count);
    CHECK_FALSE(evaluation::load_confidence_calibration_artifact(
        wrong_count_path));

    auto decreasing = *serialized;
    replace_once(decreasing, "0.80000000000000004\t0.75\t6\n",
                 "0.10000000000000001\t0.75\t6\n");
    const auto decreasing_path = temporary.path / "decreasing.tsv";
    write_file(decreasing_path, decreasing);
    CHECK_FALSE(evaluation::load_confidence_calibration_artifact(
        decreasing_path));

    const auto unterminated_path = temporary.path / "unterminated.tsv";
    write_file(unterminated_path,
               std::string_view{serialized->data(), serialized->size() - 1U});
    CHECK_FALSE(evaluation::load_confidence_calibration_artifact(
        unterminated_path));

    const auto long_line_path = temporary.path / "long-line.tsv";
    write_file(long_line_path,
               std::string(evaluation::maximum_confidence_calibration_line_bytes +
                               1U,
                           'x') +
                   "\n");
    const auto long_line =
        evaluation::load_confidence_calibration_artifact(long_line_path);
    REQUIRE_FALSE(long_line.has_value());
    CHECK(long_line.error().code ==
          evaluation::ConfidenceCalibrationArtifactErrorCode::invalid_format);

    const auto oversized_path = temporary.path / "oversized.tsv";
    write_file(oversized_path,
               std::string(
                   evaluation::maximum_confidence_calibration_artifact_bytes +
                       1U,
                   'x'));
    const auto oversized =
        evaluation::load_confidence_calibration_artifact(oversized_path);
    REQUIRE_FALSE(oversized.has_value());
    CHECK(oversized.error().code ==
          evaluation::ConfidenceCalibrationArtifactErrorCode::limit_exceeded);

    const auto directory = temporary.path / "directory";
    REQUIRE(std::filesystem::create_directory(directory));
    CHECK_FALSE(evaluation::load_confidence_calibration_artifact(directory));

    const auto occupied_staging = temporary.path / "occupied.tsv.partial";
    write_file(occupied_staging, "occupied\n");
    const auto occupied = evaluation::write_confidence_calibration_artifact(
        temporary.path / "occupied.tsv", expected);
    REQUIRE_FALSE(occupied.has_value());
    CHECK(occupied.error().code ==
          evaluation::ConfidenceCalibrationArtifactErrorCode::already_exists);

    auto invalid = expected;
    invalid.assertion.assertions_enabled = false;
    const auto invalid_serialization =
        evaluation::serialize_confidence_calibration_artifact(invalid);
    REQUIRE_FALSE(invalid_serialization.has_value());
    CHECK(invalid_serialization.error().code ==
          evaluation::ConfidenceCalibrationArtifactErrorCode::invalid_format);
}
