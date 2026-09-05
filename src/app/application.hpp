#pragma once

#include "app/file_dialog_service.hpp"

#include "app/product_settings.hpp"
#include "app/recorder_settings.hpp"
#include "app/renderer_health.hpp"
#include "app/support_bundle.hpp"
#include "app/wall_clock_report.hpp"
#include "core/clock.hpp"
#include "platform/background_shell.hpp"
#include "platform/crash_diagnostics.hpp"
#include "platform/global_hotkey.hpp"
#include "telemetry/collector.hpp"
#include "telemetry/event_collector.hpp"
#include "telemetry/provider.hpp"
#include "ui/dashboard.hpp"

#if BLACKBOX_STORAGE_ENABLED
#include "app/archive_maintenance_service.hpp"
#include "app/incident_viewer_service.hpp"
#include "storage/incident_archive.hpp"
#include "storage/incident_writer.hpp"
#if BLACKBOX_ANALYSIS_ENABLED
#include "analysis/intelligent_incident_analyzer.hpp"
#endif
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>

struct SDL_Renderer;
struct SDL_Window;

#if defined(__linux__)
namespace blackbox::platform::linux {
class LinuxAccessibilityMonitor;
}
#endif
#if defined(__APPLE__)
#include "platform/macos/macos_app_performance_monitor.hpp"
#endif

namespace blackbox::app {

struct ApplicationDiagnosticOptions {
    std::chrono::seconds runtime{};
    std::chrono::seconds capture_interval{};
    std::filesystem::path report_path{};
    bool recover_failed_incident{};
    bool overlap_automatic_capture{};
};

enum class ApplicationInitializationResult : std::uint8_t {
    ready,
    already_running,
    failed,
};

class Application final {
public:
    explicit Application(bool start_hidden = false,
                         ApplicationDiagnosticOptions diagnostic_options = {});
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    [[nodiscard]] ApplicationInitializationResult initialize();
    [[nodiscard]] int run();

private:
    void refresh_dashboard_if_due();
    void refresh_accessibility_if_due();
    void refresh_display_metrics(bool force_style_refresh = false);
    void refresh_background_shell_if_due();
    void process_background_commands(bool& running);
    void show_window() noexcept;
    void hide_window() noexcept;
    void request_incident_capture() noexcept;
    void apply_product_settings(const ui::DashboardCommand& command) noexcept;
    void request_support_bundle(const ui::DashboardCommand& command) noexcept;
    void synchronize_product_ui();
    void request_file_dialog(ui::PathField field);
    void consume_file_dialog();
    void write_diagnostic_report() noexcept;
    void write_diagnostic_progress() noexcept;
    [[nodiscard]] bool register_configured_hotkey(platform::HotkeyCombination combination) noexcept;
    void refresh_hotkey_status() noexcept;
    void shutdown() noexcept;

    core::SystemMonotonicClock telemetry_clock_{};
    std::unique_ptr<telemetry::ITelemetryProvider> telemetry_provider_{};
    std::unique_ptr<telemetry::ISystemEventProvider> system_event_provider_{};
    std::unique_ptr<telemetry::SystemEventCollector> system_event_collector_{};
#if BLACKBOX_AUTOMATIC_DETECTION_ENABLED
    std::unique_ptr<telemetry::AutomaticIncidentDetector> automatic_detector_{};
#endif
    std::unique_ptr<telemetry::TelemetryCollector> collector_{};
#if BLACKBOX_STORAGE_ENABLED
    std::unique_ptr<storage::SqliteIncidentArchive> incident_archive_{};
    std::unique_ptr<storage::IncidentWriter> incident_writer_{};
    std::unique_ptr<ArchiveMaintenanceService> archive_maintenance_service_{};
#if BLACKBOX_ANALYSIS_ENABLED
    std::unique_ptr<analysis::IntelligentIncidentAnalyzer> incident_analyzer_{};
#endif
    std::unique_ptr<IncidentViewerService> incident_viewer_service_{};
#endif
    std::unique_ptr<platform::IGlobalHotkeyManager> hotkey_manager_{};
    std::unique_ptr<platform::IBackgroundShell> background_shell_{};
    std::unique_ptr<platform::ICrashDiagnostics> crash_diagnostics_{};
#if defined(__linux__)
    std::unique_ptr<platform::linux::LinuxAccessibilityMonitor> linux_accessibility_monitor_{};
#endif
#if defined(__APPLE__)
    std::unique_ptr<platform::macos::MacosAppPerformanceMonitor>
        macos_app_performance_monitor_{};
#endif
    SupportBundleService support_bundle_service_{};
    RendererHealthTracker renderer_health_{};
    std::atomic<std::uint32_t> pending_background_commands_{};
    ui::DashboardState dashboard_state_{};
    ui::IncidentViewerState incident_viewer_state_{};
    ui::ProductUiState product_ui_state_{};
    ui::ProductPreferences saved_product_preferences_{};
    FileDialogService file_dialog_service_{};
    core::MonotonicTimePoint next_dashboard_refresh_at_{};
    core::MonotonicTimePoint next_accessibility_refresh_at_{};
    std::uint64_t dashboard_projection_collection_count_{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t dashboard_gpu_inventory_collection_count_{
        std::numeric_limits<std::uint64_t>::max()};
    std::string_view provider_name_{"Not configured"};
    std::string hotkey_status_{"Unsupported"};
    std::string hotkey_display_name_{};
    std::string background_status_text_{"Unavailable"};
    std::string storage_status_text_{"Disabled"};
    std::filesystem::path recorder_settings_path_{};
    std::filesystem::path product_settings_path_{};
    ProductSettings product_settings_{};
    std::string recorder_settings_status_text_{"Conservative defaults"};
    std::uint64_t stored_incidents_at_start_{};
    std::uint64_t viewer_last_writer_successes_{};
    std::uint64_t background_last_writer_successes_{};
    std::uint64_t background_last_writer_failures_{};
    std::uint64_t background_last_automatic_triggers_{};
    std::uint64_t background_last_snapshot_failures_{};
    std::uint64_t background_last_capture_cancellations_{};
    std::uint64_t archive_maintenance_generation_{};
    std::uint64_t archive_content_epoch_{};
    core::MonotonicTimePoint next_background_refresh_at_{};

    SDL_Window* window_{nullptr};
    SDL_Renderer* renderer_{nullptr};
    bool sdl_initialized_{false};
    bool imgui_initialized_{false};
    bool implot_initialized_{false};
    bool imgui_sdl_backend_initialized_{false};
    bool imgui_renderer_backend_initialized_{false};
    bool start_hidden_{};
    bool background_shell_started_{};
    bool background_launch_at_login_enabled_{};
    bool recorder_paused_{};
    bool high_contrast_enabled_{};
    bool animations_enabled_{true};
    float display_scale_{1.0F};
    ApplicationDiagnosticOptions diagnostic_options_{};
    core::MonotonicTimePoint diagnostic_monotonic_anchor_{};
    std::chrono::system_clock::time_point diagnostic_utc_anchor_{};
    platform::BackgroundShellDiagnostics final_shell_diagnostics_{};
    platform::CrashDiagnosticsSnapshot final_crash_diagnostics_{};
    bool diagnostic_started_{};
    bool diagnostic_completed_{};
    bool diagnostic_report_written_{};
    bool diagnostic_report_failed_{};
    bool diagnostic_recovery_requested_{};
    bool diagnostic_overlap_requested_{};
    core::MonotonicTimePoint next_diagnostic_progress_at_{};
    bool shutdown_started_{};
};

} // namespace blackbox::app
