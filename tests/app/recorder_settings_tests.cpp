#include "app/recorder_settings.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace app = blackbox::app;
namespace telemetry = blackbox::telemetry;
using namespace std::chrono_literals;

namespace {

class TemporarySettings final {
public:
    TemporarySettings() {
        static std::atomic<std::uint64_t> sequence{};
        directory = std::filesystem::temp_directory_path() /
                    ("blackbox-settings-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(sequence.fetch_add(1U)));
        path = directory / "settings.ini";
    }
    ~TemporarySettings() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    std::filesystem::path directory{};
    std::filesystem::path path{};
};

} // namespace

TEST_CASE("recorder settings use defaults when absent and round-trip validated profiles",
          "[app][settings][configuration]") {
    TemporarySettings temporary;
    const auto defaults = app::load_recorder_settings(temporary.path);
    REQUIRE(defaults.has_value());
    CHECK(defaults->values.sample_interval == 1s);
    CHECK(defaults->values.history_duration == 5min);

    auto values = defaults->values;
    values.sample_interval = 250ms;
    values.history_duration = 2min;
    values.collect_process_paths = false;
    const auto detailed = telemetry::validate_recorder_configuration(values);
    REQUIRE(detailed.has_value());
    REQUIRE(app::save_recorder_settings(temporary.path, *detailed).has_value());
    const auto loaded = app::load_recorder_settings(temporary.path);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == *detailed);
    CHECK(std::filesystem::file_size(temporary.path) <=
          app::maximum_recorder_settings_bytes);

    auto backup = temporary.path;
    backup += ".bak";
    std::filesystem::rename(temporary.path, backup);
    const auto recovered = app::load_recorder_settings(temporary.path);
    REQUIRE(recovered.has_value());
    CHECK(*recovered == *detailed);
}

TEST_CASE("recorder settings reject malformed, unknown, and out-of-bound input",
          "[app][settings][failure]") {
    TemporarySettings temporary;
    std::filesystem::create_directories(temporary.directory);
    {
        std::ofstream output{temporary.path};
        output << "format=1\nsample_interval_ms=0\n";
    }
    const auto incomplete = app::load_recorder_settings(temporary.path);
    REQUIRE_FALSE(incomplete.has_value());
    CHECK(incomplete.error().code == app::RecorderSettingsErrorCode::invalid_format);

    {
        std::ofstream output{temporary.path, std::ios::trunc};
        output << "format=1\n"
                  "sample_interval_ms=1\n"
                  "history_duration_ms=100000000\n"
                  "late_tolerance_ms=50\n"
                  "metadata_interval_ms=30000\n"
                  "incident_pre_window_ms=120000\n"
                  "incident_post_window_ms=30000\n"
                  "resume_gap_threshold_ms=5000\n"
                  "collect_process_paths=1\n";
    }
    const auto excessive = app::load_recorder_settings(temporary.path);
    REQUIRE_FALSE(excessive.has_value());
    CHECK(excessive.error().code == app::RecorderSettingsErrorCode::invalid_configuration);
}

TEST_CASE("recorder settings reject every non-v1 format without conversion",
          "[app][settings][direct-v1]") {
    TemporarySettings temporary;
    const auto defaults = app::load_recorder_settings(temporary.path);
    REQUIRE(defaults.has_value());
    REQUIRE(app::save_recorder_settings(temporary.path, *defaults).has_value());

    std::ifstream input{temporary.path};
    std::string contents{std::istreambuf_iterator<char>{input},
                         std::istreambuf_iterator<char>{}};
    const auto marker = contents.find("format=1");
    REQUIRE(marker != std::string::npos);
    contents.replace(marker, std::string{"format=1"}.size(), "format=2");
    {
        std::ofstream output{temporary.path, std::ios::trunc};
        output << contents;
    }

    const auto loaded = app::load_recorder_settings(temporary.path);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == app::RecorderSettingsErrorCode::invalid_format);
}
