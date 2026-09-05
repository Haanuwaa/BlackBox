#include "app/product_settings.hpp"
#include "core/environment_path.hpp"

#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace blackbox::app {
namespace {


[[nodiscard]] ProductSettingsError settings_error(const ProductSettingsErrorCode code,
                                                  std::string message) {
    return ProductSettingsError{code, std::move(message)};
}

template <typename Integer>
[[nodiscard]] bool parse_integer(const std::string_view text, Integer& output) noexcept {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_boolean(const std::string_view text, bool& output) noexcept {
    if (text == "0") {
        output = false;
        return true;
    }
    if (text == "1") {
        output = true;
        return true;
    }
    return false;
}

[[nodiscard]] std::string path_text(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] bool safe_field(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= 1'024U &&
           value.find_first_of("\r\n=") == std::string_view::npos;
}

} // namespace

std::filesystem::path default_product_settings_path() {
    if (const auto override_path = core::environment_path("BLACKBOX_PRODUCT_SETTINGS_PATH")) {
        return std::filesystem::path{*override_path};
    }
    if (const auto local_app_data = core::environment_path("LOCALAPPDATA")) {
        return std::filesystem::path{*local_app_data} / "BlackBox" / "product-settings.ini";
    }
    if (const auto config_home = core::environment_path("XDG_CONFIG_HOME")) {
        return std::filesystem::path{*config_home} / "blackbox" / "product-settings.ini";
    }
    if (const auto home = core::environment_path("HOME")) {
        return std::filesystem::path{*home} / ".config" / "blackbox" / "product-settings.ini";
    }
    return std::filesystem::current_path() / "blackbox-data" / "product-settings.ini";
}

ProductSettings default_product_settings() {
    auto result = ProductSettings{};
    auto base = default_product_settings_path().parent_path();
    if (base.empty()) {
        base = std::filesystem::current_path() / "blackbox-data";
    }
    result.archive_path = base / "incidents.sqlite3";
    return result;
}

std::expected<ProductSettings, ProductSettingsError>
validate_product_settings(ProductSettings settings) noexcept {
    try {
        const auto key = static_cast<std::uint8_t>(settings.incident_hotkey.key);
        if (key < static_cast<std::uint8_t>(platform::HotkeyKey::f1) ||
            key > static_cast<std::uint8_t>(platform::HotkeyKey::f12) ||
            (!settings.incident_hotkey.control && !settings.incident_hotkey.shift &&
             !settings.incident_hotkey.alt && !settings.incident_hotkey.system_modifier)) {
            return std::unexpected{
                settings_error(ProductSettingsErrorCode::invalid_configuration,
                               "hotkey requires F1-F12 and at least one modifier")};
        }
        if (settings.detector_sensitivity < DetectorSensitivity::conservative ||
            settings.detector_sensitivity > DetectorSensitivity::sensitive) {
            return std::unexpected{settings_error(ProductSettingsErrorCode::invalid_configuration,
                                                  "detector sensitivity is invalid")};
        }
        if (settings.automatic_detection_enabled && !settings.detect_cpu &&
            !settings.detect_memory && !settings.detect_disk && !settings.detect_network) {
            return std::unexpected{
                settings_error(ProductSettingsErrorCode::invalid_configuration,
                               "automatic detection requires at least one resource")};
        }
        if (settings.detector_cooldown < std::chrono::seconds::zero() ||
            settings.detector_cooldown > std::chrono::hours{24}) {
            return std::unexpected{
                settings_error(ProductSettingsErrorCode::invalid_configuration,
                               "detector cooldown must be between 0 seconds and 24 hours")};
        }
        const auto archive = path_text(settings.archive_path);
        if (!settings.archive_path.is_absolute() || !settings.archive_path.has_filename() ||
            !safe_field(archive)) {
            return std::unexpected{settings_error(ProductSettingsErrorCode::invalid_configuration,
                                                  "archive path must be an absolute file path of "
                                                  "at most 1024 bytes")};
        }
        if (settings.archive_maximum_bytes < minimum_archive_bytes ||
            settings.archive_maximum_bytes > maximum_archive_bytes) {
            return std::unexpected{
                settings_error(ProductSettingsErrorCode::invalid_configuration,
                               "archive capacity must be between 64 MiB and 64 GiB")};
        }
        return settings;
    } catch (const std::exception& exception) {
        return std::unexpected{
            settings_error(ProductSettingsErrorCode::invalid_configuration, exception.what())};
    } catch (...) {
        return std::unexpected{settings_error(ProductSettingsErrorCode::invalid_configuration,
                                              "unknown product settings validation failure")};
    }
}

std::expected<ProductSettings, ProductSettingsError>
parse_product_settings_text(const std::string_view text) noexcept {
    try {
        if (text.empty() || text.size() > maximum_product_settings_bytes) {
            return std::unexpected{settings_error(ProductSettingsErrorCode::invalid_format,
                                                  "product settings text has an invalid size")};
        }
        std::istringstream stream{std::string{text}};
        std::map<std::string, std::string, std::less<>> fields;
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const auto separator = line.find('=');
            if (separator == std::string::npos || separator == 0U ||
                separator + 1U == line.size() ||
                !fields.emplace(line.substr(0U, separator), line.substr(separator + 1U)).second) {
                return std::unexpected{settings_error(ProductSettingsErrorCode::invalid_format,
                                                      "product settings contain an invalid field")};
            }
        }
        constexpr std::array<std::string_view, 22U> required{"format",
                                                             "hotkey_key",
                                                             "hotkey_control",
                                                             "hotkey_shift",
                                                             "hotkey_alt",
                                                             "hotkey_system_modifier",
                                                             "automatic_detection",
                                                             "detector_sensitivity",
                                                             "detect_cpu",
                                                             "detect_memory",
                                                             "detect_disk",
                                                             "detect_network",
                                                             "detector_cooldown_seconds",
                                                             "notifications",
                                                             "record_foreground_application",
                                                             "record_process_lifecycle",
                                                             "record_power_and_device_events",
                                                             "record_audio_device_events",
                                                             "record_system_event_evidence",
                                                             "archive_path",
                                                             "archive_maximum_bytes",
                                                             "onboarding_completed"};
        std::uint32_t format{};
        if (!fields.contains("format") || !parse_integer(fields["format"], format) ||
            format != product_settings_format_version) {
            return std::unexpected{settings_error(ProductSettingsErrorCode::invalid_format,
                                                  "product settings format is unsupported")};
        }
        if (fields.size() != required.size()) {
            return std::unexpected{
                settings_error(ProductSettingsErrorCode::invalid_format,
                               "product settings contain unknown or missing fields")};
        }
        const auto contains_required = [&fields](const auto& keys) {
            for (const auto key : keys) {
                if (!fields.contains(key)) return false;
            }
            return true;
        };
        if (!contains_required(required)) {
            return std::unexpected{
                settings_error(ProductSettingsErrorCode::invalid_format,
                               "product settings contain unknown or missing fields")};
        }
        std::uint32_t hotkey_key{};
        std::uint32_t sensitivity{};
        std::uint64_t cooldown{};
        std::uint64_t archive_maximum{};
        ProductSettings settings{};
        if (!parse_integer(fields["format"], format) ||
            !parse_integer(fields["hotkey_key"], hotkey_key) ||
            !parse_boolean(fields["hotkey_control"], settings.incident_hotkey.control) ||
            !parse_boolean(fields["hotkey_shift"], settings.incident_hotkey.shift) ||
            !parse_boolean(fields["hotkey_alt"], settings.incident_hotkey.alt) ||
            !parse_boolean(fields["hotkey_system_modifier"],
                           settings.incident_hotkey.system_modifier) ||
            !parse_boolean(fields["automatic_detection"], settings.automatic_detection_enabled) ||
            !parse_integer(fields["detector_sensitivity"], sensitivity) ||
            !parse_boolean(fields["detect_cpu"], settings.detect_cpu) ||
            !parse_boolean(fields["detect_memory"], settings.detect_memory) ||
            !parse_boolean(fields["detect_disk"], settings.detect_disk) ||
            !parse_boolean(fields["detect_network"], settings.detect_network) ||
            !parse_integer(fields["detector_cooldown_seconds"], cooldown) ||
            !parse_boolean(fields["notifications"], settings.notifications_enabled) ||
            !parse_integer(fields["archive_maximum_bytes"], archive_maximum) ||
            !parse_boolean(fields["onboarding_completed"], settings.onboarding_completed)) {
            return std::unexpected{settings_error(ProductSettingsErrorCode::invalid_format,
                                                  "product settings contain an invalid value")};
        }
        if (!parse_boolean(fields["record_foreground_application"],
                           settings.record_foreground_application) ||
            !parse_boolean(fields["record_process_lifecycle"], settings.record_process_lifecycle) ||
            !parse_boolean(fields["record_power_and_device_events"],
                           settings.record_power_and_device_events) ||
            !parse_boolean(fields["record_audio_device_events"],
                           settings.record_audio_device_events) ||
            !parse_boolean(fields["record_system_event_evidence"],
                           settings.record_system_event_evidence)) {
            return std::unexpected{
                settings_error(ProductSettingsErrorCode::invalid_format,
                               "product settings contain an invalid evidence privacy value")};
        }
        settings.incident_hotkey.key = static_cast<platform::HotkeyKey>(hotkey_key);
        settings.detector_sensitivity = static_cast<DetectorSensitivity>(sensitivity);
        settings.detector_cooldown = std::chrono::seconds{cooldown};
        settings.archive_maximum_bytes = archive_maximum;
        const auto& archive_text = fields["archive_path"];
        settings.archive_path = std::filesystem::path{std::u8string{
            reinterpret_cast<const char8_t*>(archive_text.data()), archive_text.size()}};
        return validate_product_settings(std::move(settings));
    } catch (const std::exception& exception) {
        return std::unexpected{
            settings_error(ProductSettingsErrorCode::invalid_format, exception.what())};
    } catch (...) {
        return std::unexpected{settings_error(ProductSettingsErrorCode::invalid_format,
                                              "unknown product settings parse failure")};
    }
}

std::expected<ProductSettings, ProductSettingsError>
load_product_settings(const std::filesystem::path& path) noexcept {
    try {
        auto source = path;
        auto backup = path;
        backup += ".bak";
        if (!std::filesystem::exists(source) && std::filesystem::exists(backup)) {
            source = std::move(backup);
        }
        if (!std::filesystem::exists(source)) {
            return validate_product_settings(default_product_settings());
        }
        const auto size = std::filesystem::file_size(source);
        if (size == 0U || size > maximum_product_settings_bytes) {
            return std::unexpected{settings_error(ProductSettingsErrorCode::invalid_format,
                                                  "product settings file has an invalid size")};
        }
        std::ifstream stream{source, std::ios::binary};
        if (!stream) {
            return std::unexpected{settings_error(ProductSettingsErrorCode::cannot_read,
                                                  "cannot open product settings")};
        }
        std::string text(static_cast<std::size_t>(size), '\0');
        stream.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream || stream.gcount() != static_cast<std::streamsize>(text.size())) {
            return std::unexpected{settings_error(ProductSettingsErrorCode::cannot_read,
                                                  "cannot read complete product settings")};
        }
        return parse_product_settings_text(text);
    } catch (const std::exception& exception) {
        return std::unexpected{
            settings_error(ProductSettingsErrorCode::cannot_read, exception.what())};
    } catch (...) {
        return std::unexpected{settings_error(ProductSettingsErrorCode::cannot_read,
                                              "unknown product settings read failure")};
    }
}

std::expected<void, ProductSettingsError>
save_product_settings(const std::filesystem::path& path, const ProductSettings& settings) noexcept {
    try {
        const auto validated = validate_product_settings(settings);
        if (!validated) {
            return std::unexpected{validated.error()};
        }
        std::ostringstream contents;
        contents << "format=" << product_settings_format_version << '\n'
                 << "hotkey_key=" << static_cast<unsigned>(settings.incident_hotkey.key) << '\n'
                 << "hotkey_control=" << settings.incident_hotkey.control << '\n'
                 << "hotkey_shift=" << settings.incident_hotkey.shift << '\n'
                 << "hotkey_alt=" << settings.incident_hotkey.alt << '\n'
                 << "hotkey_system_modifier=" << settings.incident_hotkey.system_modifier << '\n'
                 << "automatic_detection=" << settings.automatic_detection_enabled << '\n'
                 << "detector_sensitivity=" << static_cast<unsigned>(settings.detector_sensitivity)
                 << '\n'
                 << "detect_cpu=" << settings.detect_cpu << '\n'
                 << "detect_memory=" << settings.detect_memory << '\n'
                 << "detect_disk=" << settings.detect_disk << '\n'
                 << "detect_network=" << settings.detect_network << '\n'
                 << "detector_cooldown_seconds=" << settings.detector_cooldown.count() << '\n'
                 << "notifications=" << settings.notifications_enabled << '\n'
                 << "record_foreground_application=" << settings.record_foreground_application
                 << '\n'
                 << "record_process_lifecycle=" << settings.record_process_lifecycle << '\n'
                 << "record_power_and_device_events=" << settings.record_power_and_device_events
                 << '\n'
                 << "record_audio_device_events=" << settings.record_audio_device_events << '\n'
                 << "record_system_event_evidence=" << settings.record_system_event_evidence << '\n'
                 << "archive_path=" << path_text(settings.archive_path) << '\n'
                 << "archive_maximum_bytes=" << settings.archive_maximum_bytes << '\n'
                 << "onboarding_completed=" << settings.onboarding_completed << '\n';

        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
        auto temporary = path;
        temporary += ".tmp";
        auto backup = path;
        backup += ".bak";
        {
            std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
            if (!stream || !(stream << contents.str()) || !stream.flush()) {
                return std::unexpected{settings_error(ProductSettingsErrorCode::cannot_write,
                                                      "cannot write temporary product settings")};
            }
        }
        std::error_code filesystem_error;
        std::filesystem::remove(backup, filesystem_error);
        filesystem_error.clear();
        const auto had_existing = std::filesystem::exists(path);
        if (had_existing) {
            std::filesystem::rename(path, backup, filesystem_error);
            if (filesystem_error) {
                return std::unexpected{settings_error(ProductSettingsErrorCode::cannot_write,
                                                      "cannot stage existing product settings")};
            }
        }
        std::filesystem::rename(temporary, path, filesystem_error);
        if (filesystem_error) {
            if (had_existing) {
                std::error_code restore_error;
                std::filesystem::rename(backup, path, restore_error);
            }
            return std::unexpected{settings_error(ProductSettingsErrorCode::cannot_write,
                                                  "cannot replace product settings")};
        }
        std::filesystem::remove(backup, filesystem_error);
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected{
            settings_error(ProductSettingsErrorCode::cannot_write, exception.what())};
    } catch (...) {
        return std::unexpected{settings_error(ProductSettingsErrorCode::cannot_write,
                                              "unknown product settings write failure")};
    }
}

telemetry::AutomaticDetectorConfiguration
detector_configuration(const ProductSettings& settings) noexcept {
    auto configuration = telemetry::AutomaticDetectorConfiguration{};
    configuration.cpu_enabled = settings.detect_cpu;
    configuration.memory_enabled = settings.detect_memory;
    configuration.disk_enabled = settings.detect_disk;
    configuration.network_enabled = settings.detect_network;
    configuration.cooldown = settings.detector_cooldown;
    switch (settings.detector_sensitivity) {
    case DetectorSensitivity::conservative:
        configuration.consecutive_samples = 4U;
        configuration.statistical_z_score = 10.0;
        configuration.disk_stall_seconds = 0.200;
        configuration.disk_queue_depth = 16.0;
        configuration.network_retransmit_fraction = 0.50;
        configuration.network_failure_events = 4U;
        break;
    case DetectorSensitivity::balanced:
        break;
    case DetectorSensitivity::sensitive:
        configuration.consecutive_samples = 2U;
        configuration.statistical_z_score = 6.0;
        configuration.disk_stall_seconds = 0.050;
        configuration.disk_queue_depth = 4.0;
        configuration.network_retransmit_fraction = 0.10;
        configuration.network_failure_events = 1U;
        break;
    }
    return configuration;
}

telemetry::EventProviderConfiguration
event_provider_configuration(const ProductSettings& settings) noexcept {
    telemetry::EventProviderConfiguration result{};
    result.power_events = settings.record_power_and_device_events;
    result.device_events = settings.record_power_and_device_events;
    result.audio_device_events = settings.record_audio_device_events;
    result.service_events = settings.record_system_event_evidence;
    result.security_events = settings.record_system_event_evidence;
    result.update_events = settings.record_system_event_evidence;
    result.application_events = settings.record_system_event_evidence;
    result.network_events = settings.record_system_event_evidence;
    result.graphics_events = settings.record_system_event_evidence;
    result.storage_events = settings.record_system_event_evidence;
    return result;
}

} // namespace blackbox::app
