#include "app/product_settings.hpp"
#include "app/recorder_settings.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace app = blackbox::app;

namespace {

[[nodiscard]] std::string product_seed() {
    const auto archive = (std::filesystem::temp_directory_path() /
                          "blackbox-property-archive.sqlite3").generic_string();
    return
        "format=1\n"
        "hotkey_key=12\n"
        "hotkey_control=1\n"
        "hotkey_shift=1\n"
        "hotkey_alt=0\n"
        "hotkey_windows=0\n"
        "automatic_detection=1\n"
        "detector_sensitivity=1\n"
        "detect_cpu=1\n"
        "detect_memory=1\n"
        "detect_disk=1\n"
        "detect_network=1\n"
        "detector_cooldown_seconds=120\n"
        "notifications=1\n"
        "record_foreground_application=0\n"
        "record_process_lifecycle=0\n"
        "record_power_and_device_events=0\n"
        "record_audio_device_events=0\n"
        "record_windows_event_log_evidence=0\n"
        "archive_path=" + archive + "\n"
        "archive_maximum_bytes=1073741824\n"
        "onboarding_completed=0\n";
}

constexpr std::string_view recorder_seed =
    "format=1\n"
    "sample_interval_ms=1000\n"
    "history_duration_ms=300000\n"
    "late_tolerance_ms=250\n"
    "metadata_interval_ms=30000\n"
    "incident_pre_window_ms=120000\n"
    "incident_post_window_ms=30000\n"
    "resume_gap_threshold_ms=5000\n"
    "collect_process_paths=1\n";

template <typename Parser, typename Validator>
void exercise_mutations(const std::string& seed, Parser&& parse,
                        Validator&& validate) {
    const auto accepted_seed = parse(seed);
    REQUIRE(accepted_seed.has_value());
    CHECK(validate(*accepted_seed));

    std::size_t attempts{};
    for (std::size_t length = 0U; length <= seed.size(); ++length) {
        const auto result = parse(std::string_view{seed}.substr(0U, length));
        if (result) CHECK(validate(*result));
        ++attempts;
    }

    constexpr std::array<char, 6U> replacements{
        '\0', '\n', '\r', '=', static_cast<char>(0x7F), static_cast<char>(0xFF)};
    auto mutation = seed;
    for (std::size_t index = 0U; index < seed.size(); ++index) {
        const auto original = mutation[index];
        for (const auto replacement : replacements) {
            mutation[index] = replacement;
            const auto result = parse(mutation);
            if (result) CHECK(validate(*result));
            ++attempts;
        }
        mutation[index] = original;
    }

    mutation = seed + seed;
    CHECK_FALSE(parse(mutation));
    ++attempts;
    mutation.assign(20'000U, 'x');
    CHECK_FALSE(parse(mutation));
    ++attempts;
    CHECK(attempts > 1'000U);
}

} // namespace

TEST_CASE("strict direct-v1 settings parsers survive bounded deterministic mutations",
          "[property][settings][format-v1]") {
    exercise_mutations(
        product_seed(),
        [](const std::string_view text) {
            return app::parse_product_settings_text(text);
        },
        [](const app::ProductSettings& value) {
            return app::validate_product_settings(value).has_value();
        });

    exercise_mutations(
        std::string{recorder_seed},
        [](const std::string_view text) {
            return app::parse_recorder_settings_text(text);
        },
        [](const blackbox::telemetry::ValidatedRecorderConfiguration& value) {
            return blackbox::telemetry::validate_recorder_configuration(value.values)
                .has_value();
        });
}
