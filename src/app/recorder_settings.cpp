#include "app/recorder_settings.hpp"

#include <array>
#include <charconv>
#include <chrono>
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

[[nodiscard]] std::optional<std::string> environment_value(const char* name) {
    std::size_t required{};
#if defined(_WIN32)
    if (getenv_s(&required, nullptr, 0U, name) != 0 || required == 0U) return std::nullopt;
    std::string value(required - 1U, '\0');
    if (getenv_s(&required, value.data(), required, name) != 0) return std::nullopt;
    return value;
#else
    const auto* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? std::nullopt
                                               : std::optional<std::string>{value};
#endif
}

[[nodiscard]] RecorderSettingsError error(const RecorderSettingsErrorCode code,
                                          std::string message) {
    return RecorderSettingsError{code, std::move(message)};
}

[[nodiscard]] bool parse_nonnegative(const std::string_view text,
                                     std::int64_t& output) noexcept {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && output >= 0;
}

} // namespace

std::filesystem::path default_recorder_settings_path() {
    if (const auto override_path = environment_value("BLACKBOX_SETTINGS_PATH")) {
        return std::filesystem::path{*override_path};
    }
    if (const auto local_app_data = environment_value("LOCALAPPDATA")) {
        return std::filesystem::path{*local_app_data} / "BlackBox" / "settings.ini";
    }
    if (const auto config_home = environment_value("XDG_CONFIG_HOME")) {
        return std::filesystem::path{*config_home} / "blackbox" / "settings.ini";
    }
    if (const auto home = environment_value("HOME")) {
        return std::filesystem::path{*home} / ".config" / "blackbox" / "settings.ini";
    }
    return std::filesystem::current_path() / "blackbox-data" / "settings.ini";
}

std::expected<telemetry::ValidatedRecorderConfiguration, RecorderSettingsError>
parse_recorder_settings_text(const std::string_view text) noexcept {
    try {
        if (text.empty() || text.size() > maximum_recorder_settings_bytes) {
            return std::unexpected{error(RecorderSettingsErrorCode::invalid_format,
                                         "recorder settings text has an invalid size")};
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
                return std::unexpected{error(RecorderSettingsErrorCode::invalid_format,
                                             "recorder settings contain an invalid field")};
            }
        }
        constexpr std::array<std::string_view, 9U> required_fields{
            "format", "sample_interval_ms", "history_duration_ms", "late_tolerance_ms",
            "metadata_interval_ms", "incident_pre_window_ms", "incident_post_window_ms",
            "resume_gap_threshold_ms", "collect_process_paths"};
        const auto format_field = fields.find("format");
        std::int64_t format{};
        if (format_field == fields.end() ||
            !parse_nonnegative(format_field->second, format) ||
            format != recorder_settings_format_version) {
            return std::unexpected{error(RecorderSettingsErrorCode::invalid_format,
                                         "unsupported recorder settings format")};
        }
        if (fields.size() != required_fields.size()) {
            return std::unexpected{error(RecorderSettingsErrorCode::invalid_format,
                                         "recorder settings contain unknown or missing fields")};
        }
        std::array<std::int64_t, 8U> values{};
        for (std::size_t index = 0U; index < values.size(); ++index) {
            const auto found = fields.find(required_fields[index]);
            if (found == fields.end() || !parse_nonnegative(found->second, values[index])) {
                return std::unexpected{error(RecorderSettingsErrorCode::invalid_format,
                                             "recorder settings contain a non-numeric value")};
            }
        }
        telemetry::RecorderConfiguration configuration{};
        configuration.sample_interval = std::chrono::milliseconds{values[1]};
        configuration.history_duration = std::chrono::milliseconds{values[2]};
        configuration.late_tolerance = std::chrono::milliseconds{values[3]};
        configuration.metadata_interval = std::chrono::milliseconds{values[4]};
        configuration.incident_pre_window = std::chrono::milliseconds{values[5]};
        configuration.incident_post_window = std::chrono::milliseconds{values[6]};
        configuration.resume_gap_threshold = std::chrono::milliseconds{values[7]};
        const auto paths = fields.find("collect_process_paths");
        if (paths == fields.end() ||
            (paths->second != "0" && paths->second != "1")) {
            return std::unexpected{error(
                RecorderSettingsErrorCode::invalid_format,
                "recorder settings contain an invalid privacy value")};
        }
        configuration.collect_process_paths = paths->second == "1";
        auto validated = telemetry::validate_recorder_configuration(configuration);
        if (!validated) {
            return std::unexpected{error(RecorderSettingsErrorCode::invalid_configuration,
                                         "recorder settings exceed validated bounds")};
        }
        return *validated;
    } catch (const std::exception& exception) {
        return std::unexpected{error(RecorderSettingsErrorCode::invalid_format,
                                     exception.what())};
    } catch (...) {
        return std::unexpected{error(RecorderSettingsErrorCode::invalid_format,
                                     "unknown recorder settings parse failure")};
    }
}

std::expected<telemetry::ValidatedRecorderConfiguration, RecorderSettingsError>
load_recorder_settings(const std::filesystem::path& path) noexcept {
    try {
        auto source = path;
        auto backup = path;
        backup += ".bak";
        if (!std::filesystem::exists(source) && std::filesystem::exists(backup)) {
            source = std::move(backup);
        }
        if (!std::filesystem::exists(source)) {
            auto defaults = telemetry::validate_recorder_configuration({});
            if (!defaults) {
                return std::unexpected{error(RecorderSettingsErrorCode::invalid_configuration,
                                             "built-in recorder settings are invalid")};
            }
            return *defaults;
        }
        const auto size = std::filesystem::file_size(source);
        if (size == 0U || size > maximum_recorder_settings_bytes) {
            return std::unexpected{error(RecorderSettingsErrorCode::invalid_format,
                                         "recorder settings file has an invalid size")};
        }
        std::ifstream stream{source, std::ios::binary};
        if (!stream) {
            return std::unexpected{error(RecorderSettingsErrorCode::cannot_read,
                                         "cannot open recorder settings")};
        }
        std::string text(static_cast<std::size_t>(size), '\0');
        stream.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream || stream.gcount() != static_cast<std::streamsize>(text.size())) {
            return std::unexpected{error(RecorderSettingsErrorCode::cannot_read,
                                         "cannot read complete recorder settings")};
        }
        return parse_recorder_settings_text(text);
    } catch (const std::exception& exception) {
        return std::unexpected{error(RecorderSettingsErrorCode::cannot_read,
                                     exception.what())};
    } catch (...) {
        return std::unexpected{error(RecorderSettingsErrorCode::cannot_read,
                                     "unknown recorder settings read failure")};
    }
}

std::expected<void, RecorderSettingsError> save_recorder_settings(
    const std::filesystem::path& path,
    const telemetry::ValidatedRecorderConfiguration& settings) noexcept {
    try {
        const auto revalidated = telemetry::validate_recorder_configuration(settings.values);
        if (!revalidated || *revalidated != settings) {
            return std::unexpected{error(RecorderSettingsErrorCode::invalid_configuration,
                                         "refusing to save invalid recorder settings")};
        }
        const auto milliseconds = [](const std::chrono::nanoseconds value) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(value).count();
        };
        std::ostringstream contents;
        contents << "format=" << recorder_settings_format_version << '\n'
                 << "sample_interval_ms=" << milliseconds(settings.values.sample_interval) << '\n'
                 << "history_duration_ms=" << milliseconds(settings.values.history_duration) << '\n'
                 << "late_tolerance_ms=" << milliseconds(settings.values.late_tolerance) << '\n'
                 << "metadata_interval_ms=" << milliseconds(settings.values.metadata_interval) << '\n'
                 << "incident_pre_window_ms=" << milliseconds(settings.values.incident_pre_window) << '\n'
                 << "incident_post_window_ms=" << milliseconds(settings.values.incident_post_window) << '\n'
                 << "resume_gap_threshold_ms=" << milliseconds(settings.values.resume_gap_threshold) << '\n'
                 << "collect_process_paths=" << settings.values.collect_process_paths << '\n';
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
        auto temporary = path;
        temporary += ".tmp";
        auto backup = path;
        backup += ".bak";
        {
            std::ofstream stream{temporary, std::ios::trunc};
            if (!stream || !(stream << contents.str()) || !stream.flush()) {
                return std::unexpected{error(RecorderSettingsErrorCode::cannot_write,
                                             "cannot write temporary recorder settings")};
            }
        }
        std::error_code filesystem_error;
        std::filesystem::remove(backup, filesystem_error);
        filesystem_error.clear();
        const auto had_existing = std::filesystem::exists(path);
        if (had_existing) {
            std::filesystem::rename(path, backup, filesystem_error);
            if (filesystem_error) {
                return std::unexpected{error(RecorderSettingsErrorCode::cannot_write,
                                             "cannot stage existing recorder settings")};
            }
        }
        std::filesystem::rename(temporary, path, filesystem_error);
        if (filesystem_error) {
            if (had_existing) {
                std::error_code restore_error;
                std::filesystem::rename(backup, path, restore_error);
            }
            return std::unexpected{error(RecorderSettingsErrorCode::cannot_write,
                                         "cannot replace recorder settings")};
        }
        std::filesystem::remove(backup, filesystem_error);
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected{error(RecorderSettingsErrorCode::cannot_write, exception.what())};
    } catch (...) {
        return std::unexpected{error(RecorderSettingsErrorCode::cannot_write,
                                     "unknown recorder settings write failure")};
    }
}

} // namespace blackbox::app
