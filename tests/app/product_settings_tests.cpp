#include "app/product_settings.hpp"
#include "app/recorder_settings.hpp"
#include "core/environment_path.hpp"
#include "core/filesystem_text.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace app = blackbox::app;
namespace platform = blackbox::platform;
using namespace std::chrono_literals;

#if defined(_WIN32)
TEST_CASE("Windows settings overrides preserve native Unicode environment paths",
          "[app][settings][unicode][environment]") {
    struct RestoreVariable {
        const wchar_t* name;
        std::wstring original;
        ~RestoreVariable() { static_cast<void>(_wputenv_s(name, original.c_str())); }
    };
    const auto old_product = blackbox::core::environment_path("BLACKBOX_PRODUCT_SETTINGS_PATH");
    const auto old_recorder = blackbox::core::environment_path("BLACKBOX_SETTINGS_PATH");
    const RestoreVariable product_restore{L"BLACKBOX_PRODUCT_SETTINGS_PATH",
                                         old_product ? old_product->native() : L""};
    const RestoreVariable recorder_restore{L"BLACKBOX_SETTINGS_PATH",
                                          old_recorder ? old_recorder->native() : L""};
    const auto path = std::filesystem::path{u8"C:/BlackBox/\u6d4b\u8bd5 \u00e9/settings.ini"};
    REQUIRE(_wputenv_s(L"BLACKBOX_PRODUCT_SETTINGS_PATH", path.c_str()) == 0);
    REQUIRE(_wputenv_s(L"BLACKBOX_SETTINGS_PATH", path.c_str()) == 0);
    CHECK(app::default_product_settings_path() == path);
    CHECK(app::default_recorder_settings_path() == path);
    CHECK(app::default_product_settings().archive_path == path.parent_path() / "incidents.sqlite3");
}
#endif

TEST_CASE("UTF-8 UI paths preserve native Unicode across settings round trips", "[app][settings][unicode]") {
    const auto text = std::u8string{u8"C:/BlackBox/\u6d4b\u8bd5/\u00e9 space.sqlite3"};
    const std::string utf8{reinterpret_cast<const char*>(text.data()), text.size()};
    const auto path = blackbox::core::path_from_utf8(utf8);
    CHECK(path == std::filesystem::path{text});
    CHECK(blackbox::core::path_to_utf8(path) == utf8);
}

namespace {

class TemporarySettings final {
public:
    TemporarySettings() {
        static std::atomic<std::uint64_t> sequence{};
        directory = std::filesystem::temp_directory_path() /
                    ("blackbox-product-settings-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(sequence.fetch_add(1U)));
        path = directory / "product-settings.ini";
    }
    ~TemporarySettings() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    std::filesystem::path directory{};
    std::filesystem::path path{};
};

} // namespace

TEST_CASE("product settings defaults and all user controls round trip",
          "[app][product-settings][configuration]") {
    TemporarySettings temporary;
    const auto defaults = app::load_product_settings(temporary.path);
    REQUIRE(defaults.has_value());
    CHECK(defaults->archive_path.is_absolute());

    auto values = *defaults;
    values.incident_hotkey = {platform::HotkeyKey::f9, true, true, false, false};
    values.automatic_detection_enabled = false;
    values.detector_sensitivity = app::DetectorSensitivity::sensitive;
    values.detect_cpu = false;
    values.detect_memory = true;
    values.detect_disk = false;
    values.detect_network = true;
    values.detector_cooldown = 17s;
    values.notifications_enabled = false;
    values.record_foreground_application = true;
    values.record_process_lifecycle = true;
    values.record_power_and_device_events = true;
    values.record_audio_device_events = true;
    values.record_system_event_evidence = true;
    values.archive_path = blackbox::core::path_from_utf8(blackbox::core::path_to_utf8(
        temporary.directory / std::filesystem::path{u8"\u6d4b\u8bd5 \u00e9"} / "incidents.sqlite3"));
    values.archive_maximum_bytes = 512ULL << 20U;
    values.onboarding_completed = true;

    REQUIRE(app::save_product_settings(temporary.path, values).has_value());
    const auto loaded = app::load_product_settings(temporary.path);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == values);
    CHECK(std::filesystem::file_size(temporary.path) <= app::maximum_product_settings_bytes);

    auto backup = temporary.path;
    backup += ".bak";
    std::filesystem::rename(temporary.path, backup);
    const auto recovered = app::load_product_settings(temporary.path);
    REQUIRE(recovered.has_value());
    CHECK(*recovered == values);
}

TEST_CASE("product settings reject unsafe combinations", "[app][product-settings][validation]") {
    TemporarySettings temporary;
    auto values = app::default_product_settings();
    values.archive_path = temporary.directory / "incidents.sqlite3";

    values.incident_hotkey = {platform::HotkeyKey::f12, false, false, false, false};
    CHECK_FALSE(app::validate_product_settings(values));
    values.incident_hotkey = platform::default_incident_hotkey;

    values.detect_cpu = values.detect_memory = values.detect_disk = values.detect_network = false;
    CHECK_FALSE(app::validate_product_settings(values));
    values.automatic_detection_enabled = false;
    CHECK(app::validate_product_settings(values));

    values.detector_cooldown = 24h + 1s;
    CHECK_FALSE(app::validate_product_settings(values));
    values.detector_cooldown = 1min;
    values.archive_path = "relative.sqlite3";
    CHECK_FALSE(app::validate_product_settings(values));
    values.archive_path = temporary.directory / "incidents.sqlite3";
    values.archive_maximum_bytes = app::minimum_archive_bytes - 1U;
    CHECK_FALSE(app::validate_product_settings(values));
}

TEST_CASE("product settings reject malformed and unknown fields",
          "[app][product-settings][failure]") {
    TemporarySettings temporary;
    std::filesystem::create_directories(temporary.directory);
    {
        std::ofstream output{temporary.path};
        output << "format=1\nunknown=value\n";
    }
    const auto loaded = app::load_product_settings(temporary.path);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == app::ProductSettingsErrorCode::invalid_format);
}

TEST_CASE("product settings reject every non-v1 format without conversion",
          "[app][product-settings][direct-v1]") {
    TemporarySettings temporary;
    auto values = app::default_product_settings();
    values.archive_path = temporary.directory / "incidents.sqlite3";
    REQUIRE(app::save_product_settings(temporary.path, values).has_value());

    std::ifstream input{temporary.path};
    std::string contents{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const auto marker = contents.find("format=1");
    REQUIRE(marker != std::string::npos);
    contents.replace(marker, std::string{"format=1"}.size(), "format=2");
    {
        std::ofstream output{temporary.path, std::ios::trunc};
        output << contents;
    }

    const auto loaded = app::load_product_settings(temporary.path);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == app::ProductSettingsErrorCode::invalid_format);
}

TEST_CASE("product settings map sensitivity and resource privacy to detector",
          "[app][product-settings][detector]") {
    auto values = app::default_product_settings();
    values.detector_sensitivity = app::DetectorSensitivity::conservative;
    values.detect_cpu = false;
    values.detect_network = false;
    values.detector_cooldown = 42s;
    const auto conservative = app::detector_configuration(values);
    CHECK(conservative.consecutive_samples == 4U);
    CHECK(conservative.statistical_z_score == 10.0);
    CHECK_FALSE(conservative.cpu_enabled);
    CHECK_FALSE(conservative.network_enabled);
    CHECK(conservative.cooldown == 42s);

    values.detector_sensitivity = app::DetectorSensitivity::sensitive;
    const auto sensitive = app::detector_configuration(values);
    CHECK(sensitive.consecutive_samples == 2U);
    CHECK(sensitive.statistical_z_score == 6.0);
}

TEST_CASE("product settings map independently gated event evidence",
          "[app][product-settings][events][privacy]") {
    auto values = app::default_product_settings();
    auto configuration = app::event_provider_configuration(values);
    CHECK_FALSE(configuration.power_events);
    CHECK_FALSE(configuration.device_events);
    CHECK_FALSE(configuration.audio_device_events);
    CHECK_FALSE(configuration.service_events);
    CHECK_FALSE(configuration.security_events);
    CHECK_FALSE(configuration.update_events);
    CHECK_FALSE(configuration.application_events);
    CHECK_FALSE(configuration.network_events);
    CHECK_FALSE(configuration.graphics_events);
    CHECK_FALSE(configuration.storage_events);

    values.record_power_and_device_events = true;
    values.record_audio_device_events = true;
    values.record_system_event_evidence = true;
    configuration = app::event_provider_configuration(values);
    CHECK(configuration.power_events);
    CHECK(configuration.device_events);
    CHECK(configuration.audio_device_events);
    CHECK(configuration.service_events);
    CHECK(configuration.security_events);
    CHECK(configuration.update_events);
    CHECK(configuration.application_events);
    CHECK(configuration.network_events);
    CHECK(configuration.graphics_events);
    CHECK(configuration.storage_events);
}
