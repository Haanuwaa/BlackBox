#pragma once

#include "telemetry/collector.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace blackbox::app {

inline constexpr std::uint32_t recorder_settings_format_version = 1U;
inline constexpr std::uintmax_t maximum_recorder_settings_bytes = 4'096U;

enum class RecorderSettingsErrorCode : std::uint8_t {
    cannot_read,
    cannot_write,
    invalid_format,
    invalid_configuration,
};

struct RecorderSettingsError {
    RecorderSettingsErrorCode code{RecorderSettingsErrorCode::invalid_format};
    std::string message{};
    friend bool operator==(const RecorderSettingsError&,
                           const RecorderSettingsError&) = default;
};

[[nodiscard]] std::filesystem::path default_recorder_settings_path();

// A missing file selects the validated conservative default. Existing malformed files
// fail explicitly so the application can report the fallback instead of silently accepting data.
[[nodiscard]] std::expected<telemetry::ValidatedRecorderConfiguration,
                            RecorderSettingsError>
parse_recorder_settings_text(std::string_view text) noexcept;

[[nodiscard]] std::expected<telemetry::ValidatedRecorderConfiguration,
                            RecorderSettingsError>
load_recorder_settings(const std::filesystem::path& path) noexcept;

[[nodiscard]] std::expected<void, RecorderSettingsError>
save_recorder_settings(const std::filesystem::path& path,
                       const telemetry::ValidatedRecorderConfiguration& settings) noexcept;

} // namespace blackbox::app
