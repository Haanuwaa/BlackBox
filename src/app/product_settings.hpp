#pragma once

#include "platform/global_hotkey.hpp"
#include "telemetry/automatic_incident_detector.hpp"
#include "telemetry/event_provider.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace blackbox::app {

inline constexpr std::uint32_t product_settings_format_version = 1U;
inline constexpr std::uintmax_t maximum_product_settings_bytes = 16'384U;
inline constexpr std::uint64_t minimum_archive_bytes = 64ULL << 20U;
inline constexpr std::uint64_t maximum_archive_bytes = 64ULL << 30U;

enum class DetectorSensitivity : std::uint8_t {
    conservative,
    balanced,
    sensitive,
};

struct ProductSettings {
    platform::HotkeyCombination incident_hotkey{platform::default_incident_hotkey};
    bool automatic_detection_enabled{true};
    DetectorSensitivity detector_sensitivity{DetectorSensitivity::balanced};
    bool detect_cpu{true};
    bool detect_memory{true};
    bool detect_disk{true};
    bool detect_network{true};
    std::chrono::seconds detector_cooldown{std::chrono::minutes{2}};
    bool notifications_enabled{true};
    bool record_foreground_application{};
    bool record_process_lifecycle{};
    bool record_power_and_device_events{};
    bool record_audio_device_events{};
    bool record_system_event_evidence{};
    std::filesystem::path archive_path{};
    std::uint64_t archive_maximum_bytes{1ULL << 30U};
    bool onboarding_completed{};
    friend bool operator==(const ProductSettings&, const ProductSettings&) = default;
};

enum class ProductSettingsErrorCode : std::uint8_t {
    cannot_read,
    cannot_write,
    invalid_format,
    invalid_configuration,
};

struct ProductSettingsError {
    ProductSettingsErrorCode code{ProductSettingsErrorCode::invalid_format};
    std::string message{};
    friend bool operator==(const ProductSettingsError&,
                           const ProductSettingsError&) = default;
};

[[nodiscard]] std::filesystem::path default_product_settings_path();
[[nodiscard]] ProductSettings default_product_settings();
[[nodiscard]] std::expected<ProductSettings, ProductSettingsError>
validate_product_settings(ProductSettings settings) noexcept;
[[nodiscard]] std::expected<ProductSettings, ProductSettingsError>
parse_product_settings_text(std::string_view text) noexcept;
[[nodiscard]] std::expected<ProductSettings, ProductSettingsError>
load_product_settings(const std::filesystem::path& path) noexcept;
[[nodiscard]] std::expected<void, ProductSettingsError>
save_product_settings(const std::filesystem::path& path,
                      const ProductSettings& settings) noexcept;
[[nodiscard]] telemetry::AutomaticDetectorConfiguration
detector_configuration(const ProductSettings& settings) noexcept;
[[nodiscard]] telemetry::EventProviderConfiguration
event_provider_configuration(const ProductSettings& settings) noexcept;

} // namespace blackbox::app
