#include "app/application.hpp"

#include "core/logger.hpp"
#include "core/version.hpp"
#include "ui/dashboard.hpp"
#include "ui/product_ui_model.hpp"

#if defined(_WIN32)
#include "platform/windows/windows_background_shell.hpp"
#include "platform/windows/windows_accessibility.hpp"
#include "platform/windows/windows_crash_diagnostics.hpp"
#include "platform/windows/windows_global_hotkey_manager.hpp"
#include "telemetry/windows/windows_telemetry_provider.hpp"
#include "telemetry/windows/windows_system_event_provider.hpp"
#elif defined(__linux__)
#include "platform/linux/linux_background_shell.hpp"
#include "telemetry/linux/linux_telemetry_provider.hpp"
#else
#include "telemetry/mock/mock_telemetry_provider.hpp"
#endif

#include <SDL3/SDL.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <utility>

namespace blackbox::app {
namespace {

using namespace std::chrono_literals;

void load_product_font(ImGuiIO& io) {
#if defined(_WIN32)
    constexpr std::array candidates{"C:/Windows/Fonts/segoeui.ttf",
                                    "C:/Windows/Fonts/arial.ttf"};
#elif defined(__APPLE__)
    constexpr std::array candidates{"/System/Library/Fonts/SFNS.ttf",
                                    "/System/Library/Fonts/Helvetica.ttc"};
#else
    constexpr std::array candidates{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                    "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"};
#endif
    for (const auto* candidate : candidates) {
        std::error_code error{};
        if (!std::filesystem::is_regular_file(candidate, error) || error) continue;
        if (io.Fonts->AddFontFromFileTTF(candidate, 17.0F) != nullptr) return;
    }
    static_cast<void>(io.Fonts->AddFontDefault());
}

[[nodiscard]] constexpr bool any_event_source_enabled(
    const telemetry::EventProviderConfiguration& configuration) noexcept {
    return configuration.power_events || configuration.device_events ||
           configuration.audio_device_events || configuration.service_events ||
           configuration.defender_events || configuration.windows_update_events ||
           configuration.application_events || configuration.dns_client_events ||
           configuration.display_driver_events || configuration.storage_events;
}

[[nodiscard]] constexpr ui::MetricDisplayStatus display_status(
    const telemetry::MetricStatus status) noexcept {
    switch (status) {
    case telemetry::MetricStatus::available:
        return ui::MetricDisplayStatus::available;
    case telemetry::MetricStatus::unsupported:
        return ui::MetricDisplayStatus::unsupported;
    case telemetry::MetricStatus::inaccessible:
        return ui::MetricDisplayStatus::inaccessible;
    case telemetry::MetricStatus::temporarily_unavailable:
        return ui::MetricDisplayStatus::warming_up;
    }
    return ui::MetricDisplayStatus::unavailable;
}

[[nodiscard]] constexpr std::string_view provider_status_text(
    const telemetry::ProviderSampleStatus status) noexcept {
    switch (status) {
    case telemetry::ProviderSampleStatus::complete:
        return "Collecting";
    case telemetry::ProviderSampleStatus::partial:
        return "Partial sample";
    case telemetry::ProviderSampleStatus::temporarily_failed:
        return "Temporarily unavailable";
    }
    return "Unknown";
}

[[nodiscard]] constexpr double microseconds(const std::chrono::nanoseconds duration) noexcept {
    return std::chrono::duration<double, std::micro>{duration}.count();
}

[[nodiscard]] constexpr double milliseconds(const std::chrono::nanoseconds duration) noexcept {
    return std::chrono::duration<double, std::milli>{duration}.count();
}

[[nodiscard]] constexpr double seconds(const std::chrono::nanoseconds duration) noexcept {
    return std::chrono::duration<double>{duration}.count();
}

[[nodiscard]] constexpr double mebibytes_per_second(
    const telemetry::BytesPerSecond value) noexcept {
    return value.value / (1024.0 * 1024.0);
}

[[nodiscard]] constexpr std::string_view capture_phase_text(
    const core::IncidentCapturePhase phase) noexcept {
    switch (phase) {
    case core::IncidentCapturePhase::idle:
        return "Ready";
    case core::IncidentCapturePhase::collecting_post_window:
        return "Collecting post-window";
    case core::IncidentCapturePhase::constructing_snapshot:
        return "Constructing immutable snapshot";
    case core::IncidentCapturePhase::queued:
        return "Queued for incident writer";
    case core::IncidentCapturePhase::queue_full:
        return "Writer queue full";
    case core::IncidentCapturePhase::stopped:
        return "Stopped";
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::uint32_t command_bit(
    const platform::BackgroundShellCommand command) noexcept {
    return 1U << static_cast<std::uint32_t>(command);
}

} // namespace

Application::Application(const bool start_hidden,
                         ApplicationDiagnosticOptions diagnostic_options) noexcept
    : start_hidden_{start_hidden}, diagnostic_options_{std::move(diagnostic_options)} {}

Application::~Application() {
    shutdown();
}

ApplicationInitializationResult Application::initialize() {
    product_settings_path_ = default_product_settings_path();
#if defined(_WIN32)
    crash_diagnostics_ =
        std::make_unique<platform::windows::WindowsCrashDiagnostics>(
            product_settings_path_.parent_path() / "crashes");
    static_cast<void>(crash_diagnostics_->install());
#endif
    const auto loaded_product_settings = load_product_settings(product_settings_path_);
    if (loaded_product_settings) {
        product_settings_ = *loaded_product_settings;
    } else {
        const auto defaults = validate_product_settings(default_product_settings());
        if (!defaults) {
            core::Logger::write(core::LogLevel::error,
                                "Default product settings are invalid");
            return ApplicationInitializationResult::failed;
        }
        product_settings_ = *defaults;
        recorder_settings_status_text_ = "Invalid product settings; using defaults: " +
                                         loaded_product_settings.error().message;
    }
    synchronize_product_ui();
#if defined(__linux__)
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        core::Logger::write(core::LogLevel::error, SDL_GetError());
        return ApplicationInitializationResult::failed;
    }
    sdl_initialized_ = true;
    background_shell_ = std::make_unique<platform::linux::LinuxBackgroundShell>();
    const auto shell_result = background_shell_->start(
        [this](const platform::BackgroundShellCommand command) {
            pending_background_commands_.fetch_or(command_bit(command),
                                                   std::memory_order_release);
        });
    if (shell_result == platform::BackgroundShellStartResult::already_running) {
        background_shell_.reset();
        return ApplicationInitializationResult::already_running;
    }
    if (shell_result == platform::BackgroundShellStartResult::started) {
        background_shell_started_ = true;
        background_launch_at_login_enabled_ =
            background_shell_->launch_at_login_enabled();
        const auto tray_available = background_shell_->diagnostics().tray_available;
        background_status_text_ = tray_available
            ? "Desktop tray active"
            : "Desktop tray unavailable; close exits";
        background_shell_->set_notifications_enabled(
            product_settings_.notifications_enabled);
    } else {
        background_shell_.reset();
        background_status_text_ = "Desktop shell unavailable; close exits";
    }
#endif
#if defined(_WIN32)
    background_shell_ = std::make_unique<platform::windows::WindowsBackgroundShell>();
    const auto shell_result = background_shell_->start(
        [this](const platform::BackgroundShellCommand command) {
            pending_background_commands_.fetch_or(command_bit(command),
                                                   std::memory_order_release);
        });
    if (shell_result == platform::BackgroundShellStartResult::already_running) {
        background_shell_.reset();
        return ApplicationInitializationResult::already_running;
    }
    if (shell_result == platform::BackgroundShellStartResult::started) {
        background_shell_started_ = true;
        background_launch_at_login_enabled_ =
            background_shell_->launch_at_login_enabled();
        background_status_text_ = "Tray active";
        background_shell_->set_notifications_enabled(
            product_settings_.notifications_enabled);
    } else {
        background_shell_.reset();
        background_status_text_ = "Tray unavailable; close exits";
    }

    telemetry_provider_ = std::make_unique<telemetry::windows::WindowsTelemetryProvider>(
        telemetry_clock_);
    system_event_provider_ =
        std::make_unique<telemetry::windows::WindowsSystemEventProvider>();
    auto event_configuration = telemetry::EventCollectorConfiguration{};
    event_configuration.provider = event_provider_configuration(product_settings_);
    event_configuration.automatic_system_event_capture =
        product_settings_.automatic_detection_enabled;
    system_event_collector_ = std::make_unique<telemetry::SystemEventCollector>(
        *system_event_provider_, telemetry_clock_, event_configuration);
    provider_name_ = "WindowsTelemetryProvider";
#elif defined(__linux__)
    telemetry_provider_ =
        std::make_unique<telemetry::linux::LinuxTelemetryProvider>(telemetry_clock_);
    provider_name_ = "LinuxTelemetryProvider";
#else
    telemetry_provider_ = std::make_unique<telemetry::mock::MockTelemetryProvider>(
        telemetry_clock_);
    provider_name_ = "MockTelemetryProvider";
#endif

    recorder_settings_path_ = default_recorder_settings_path();
    const auto loaded_settings = load_recorder_settings(recorder_settings_path_);
    telemetry::ValidatedRecorderConfiguration configuration{};
    if (!loaded_settings) {
        recorder_settings_status_text_ = "Invalid settings; using conservative defaults: " +
                                         loaded_settings.error().message;
        const auto defaults = telemetry::validate_recorder_configuration({});
        if (!defaults) {
            core::Logger::write(core::LogLevel::error,
                                "Default recorder configuration is invalid");
            return ApplicationInitializationResult::failed;
        }
        configuration = *defaults;
    } else {
        configuration = *loaded_settings;
        recorder_settings_status_text_ = "Loaded from " + recorder_settings_path_.string();
    }
    product_ui_state_.collect_process_paths = configuration.values.collect_process_paths;
    product_ui_state_.incident_pre_window_seconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            configuration.values.incident_pre_window).count());
    product_ui_state_.incident_post_window_seconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            configuration.values.incident_post_window).count());
#if BLACKBOX_AUTOMATIC_DETECTION_ENABLED
    automatic_detector_ = std::make_unique<telemetry::AutomaticIncidentDetector>(
        detector_configuration(product_settings_));
        collector_ = std::make_unique<telemetry::TelemetryCollector>(
        *telemetry_provider_, telemetry_clock_, configuration, automatic_detector_.get(),
        system_event_collector_.get(), system_event_collector_.get());
    collector_->set_automatic_detection_enabled(
        product_settings_.automatic_detection_enabled);
#else
        collector_ = std::make_unique<telemetry::TelemetryCollector>(
        *telemetry_provider_, telemetry_clock_, configuration, nullptr,
        system_event_collector_.get(), system_event_collector_.get());
#endif
    collector_->set_foreground_application_enabled(
        product_settings_.record_foreground_application);
    collector_->set_process_lifecycle_enabled(
        product_settings_.record_process_lifecycle);
    if (system_event_collector_ != nullptr) {
        system_event_collector_->set_incident_capture_sink(collector_.get());
    }

#if BLACKBOX_STORAGE_ENABLED
    auto archive_configuration = storage::ArchiveConfiguration{};
    archive_configuration.path = product_settings_.archive_path;
    archive_configuration.maximum_bytes = product_settings_.archive_maximum_bytes;
    incident_archive_ = std::make_unique<storage::SqliteIncidentArchive>(
        std::move(archive_configuration));
    if (const auto opened = incident_archive_->open(); opened.has_value()) {
        storage_status_text_ = "Ready";
        if (const auto count = incident_archive_->incident_count(); count.has_value()) {
            stored_incidents_at_start_ = *count;
        }
    } else {
        storage_status_text_ = "Unavailable: " + opened.error().message;
    }
    incident_writer_ = std::make_unique<storage::IncidentWriter>(
        collector_->incident_work_source(), *incident_archive_);
    archive_maintenance_service_ = std::make_unique<ArchiveMaintenanceService>(
        *incident_archive_, *incident_writer_);
#if BLACKBOX_ANALYSIS_ENABLED
    incident_analyzer_ = std::make_unique<analysis::IntelligentIncidentAnalyzer>();
    incident_viewer_service_ = std::make_unique<IncidentViewerService>(
        *incident_archive_, incident_analyzer_.get(), incident_archive_.get(),
        incident_archive_.get(), incident_archive_.get());
#else
    incident_viewer_service_ = std::make_unique<IncidentViewerService>(
        *incident_archive_, nullptr, nullptr, nullptr, incident_archive_.get());
#endif
#else
    auto disabled_viewer = std::make_shared<ui::IncidentViewerContent>();
    disabled_viewer->state = ui::IncidentViewerLoadState::disabled;
    disabled_viewer->status = "Storage disabled";
    incident_viewer_state_.content = std::move(disabled_viewer);
#endif

    if (!sdl_initialized_ && !SDL_Init(SDL_INIT_VIDEO)) {
        core::Logger::write(core::LogLevel::error, SDL_GetError());
        return ApplicationInitializationResult::failed;
    }
    sdl_initialized_ = true;

    window_ = SDL_CreateWindow("BlackBox", 1100, 700,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_ == nullptr) {
        core::Logger::write(core::LogLevel::error, SDL_GetError());
        return ApplicationInitializationResult::failed;
    }

    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (renderer_ == nullptr) {
        core::Logger::write(core::LogLevel::error, SDL_GetError());
        return ApplicationInitializationResult::failed;
    }
    SDL_SetRenderVSync(renderer_, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imgui_initialized_ = true;
    ImPlot::CreateContext();
    implot_initialized_ = true;

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    load_product_font(io);
#if defined(_WIN32)
    const auto accessibility = platform::windows::accessibility_preferences();
    high_contrast_enabled_ = accessibility.high_contrast;
    animations_enabled_ = accessibility.animations_enabled;
#endif
    ui::apply_accessibility_style(high_contrast_enabled_);

    if (!ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_)) {
        core::Logger::write(core::LogLevel::error, "Failed to initialize the ImGui SDL3 backend");
        return ApplicationInitializationResult::failed;
    }
    imgui_sdl_backend_initialized_ = true;
    if (!ImGui_ImplSDLRenderer3_Init(renderer_)) {
        core::Logger::write(core::LogLevel::error, "Failed to initialize the ImGui SDL renderer backend");
        return ApplicationInitializationResult::failed;
    }
    imgui_renderer_backend_initialized_ = true;
    support_bundle_service_.start();

    dashboard_state_.platform_name = BLACKBOX_PLATFORM_NAME;
    dashboard_state_.provider_name = provider_name_;
    dashboard_state_.background_status = background_status_text_;
    dashboard_state_.accessibility_high_contrast = high_contrast_enabled_;
    dashboard_state_.accessibility_animations_enabled = animations_enabled_;
    dashboard_state_.display_scale = SDL_GetWindowDisplayScale(window_);
    next_dashboard_refresh_at_ = telemetry_clock_.now();
    next_accessibility_refresh_at_ = telemetry_clock_.now() + 1s;
    next_background_refresh_at_ = telemetry_clock_.now();
#if BLACKBOX_STORAGE_ENABLED
    incident_writer_->start();
    archive_maintenance_service_->start();
    archive_maintenance_service_->refresh();
    incident_viewer_service_->start();
    incident_viewer_service_->request_page(0U, {}, ui::IncidentListOrder::newest_first);
    incident_viewer_service_->request_recurring_incidents();
#endif
    collector_->start();
    if (system_event_collector_ != nullptr && any_event_source_enabled(
            event_provider_configuration(product_settings_))) {
        system_event_collector_->start();
    }
#if defined(_WIN32)
    hotkey_manager_ = std::make_unique<platform::windows::WindowsGlobalHotkeyManager>();
    static_cast<void>(register_configured_hotkey(product_settings_.incident_hotkey));
#endif
    dashboard_state_.hotkey_status = hotkey_status_;

    if (background_shell_started_) {
        const auto tray_available = background_shell_->diagnostics().tray_available;
        if (start_hidden_ && tray_available) {
            SDL_HideWindow(window_);
            background_shell_->set_window_visible(false);
        } else {
            background_shell_->set_window_visible(true);
            if (start_hidden_ && !tray_available) {
                background_status_text_ = "Tray unavailable; window kept visible";
                dashboard_state_.background_status = background_status_text_;
            }
        }
    }
    core::Logger::write(core::LogLevel::info, "BlackBox initialized");
    return ApplicationInitializationResult::ready;
}

int Application::run() {
    bool running = true;
    const auto diagnostic_started_at = telemetry_clock_.now();
    diagnostic_started_ = diagnostic_options_.runtime > 0s;
    const auto diagnostic_exit_at = diagnostic_started_
                                        ? diagnostic_started_at + diagnostic_options_.runtime
                                        : core::MonotonicTimePoint::max();
    auto next_diagnostic_capture = diagnostic_started_ &&
                                           diagnostic_options_.capture_interval > 0s
                                       ? diagnostic_started_at +
                                             diagnostic_options_.capture_interval
                                       : core::MonotonicTimePoint::max();
    const auto process_event = [this, &running](SDL_Event& event) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT) {
            running = false;
            return;
        }
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            event.window.windowID == SDL_GetWindowID(window_)) {
            if (background_shell_started_ &&
                background_shell_->diagnostics().tray_available) {
                hide_window();
            } else {
                running = false;
            }
        }
    };

    while (running) {
        const auto now = telemetry_clock_.now();
        if (now >= diagnostic_exit_at) {
            diagnostic_completed_ = diagnostic_started_;
            break;
        }
        if (now >= next_diagnostic_capture) {
            const auto post_window = collector_ != nullptr
                                         ? collector_->diagnostics().configuration
                                               .incident_post_window
                                         : std::chrono::nanoseconds{};
            if (now + post_window + 1s < diagnostic_exit_at) {
                request_incident_capture();
            }
            do {
                next_diagnostic_capture += diagnostic_options_.capture_interval;
            } while (next_diagnostic_capture <= now);
        }
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            process_event(event);
        }
        if (!running) {
            break;
        }

        process_background_commands(running);
        refresh_background_shell_if_due();
        if (!running) {
            break;
        }

        const auto window_flags = SDL_GetWindowFlags(window_);
        if ((window_flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0U) {
            // Hidden operation avoids dashboard snapshots, process-table copies,
            // and rendering. The lightweight shell refresh remains bounded at 4 Hz.
            if (SDL_WaitEventTimeout(&event, 250)) {
                process_event(event);
            }
            continue;
        }

        refresh_dashboard_if_due();

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const auto command = ui::render_dashboard(
            dashboard_state_, incident_viewer_state_, product_ui_state_);
        switch (command.action) {
        case ui::DashboardAction::none:
            break;
        case ui::DashboardAction::capture_incident:
            request_incident_capture();
            break;
        case ui::DashboardAction::apply_recorder_settings: {
            auto values = collector_->diagnostics().configuration;
            values.sample_interval = std::chrono::milliseconds{
                command.sample_interval_milliseconds};
            values.history_duration = std::chrono::seconds{
                command.history_duration_seconds};
            auto validated = telemetry::validate_recorder_configuration(values);
            if (!validated) {
                recorder_settings_status_text_ = "Rejected: settings exceed recorder bounds";
                break;
            }
            collector_->reconfigure(*validated);
            if (const auto saved = save_recorder_settings(recorder_settings_path_, *validated);
                !saved) {
                recorder_settings_status_text_ = "Applied for this session; save failed: " +
                                                 saved.error().message;
            } else {
                recorder_settings_status_text_ = "Applied and saved to " +
                                                 recorder_settings_path_.string();
            }
            break;
        }
        case ui::DashboardAction::apply_product_settings:
            apply_product_settings(command);
            break;
        case ui::DashboardAction::complete_onboarding:
            product_settings_.onboarding_completed = true;
            product_ui_state_.onboarding_open = false;
            if (const auto saved = save_product_settings(product_settings_path_,
                                                         product_settings_); !saved) {
                recorder_settings_status_text_ = "Onboarding completed; save failed: " +
                                                 saved.error().message;
            }
            break;
        case ui::DashboardAction::create_support_bundle:
            request_support_bundle(command);
            break;
#if BLACKBOX_STORAGE_ENABLED
        case ui::DashboardAction::refresh_incidents:
            incident_viewer_service_->request_page(command.incident_offset, command.search,
                                                    command.incident_order);
            break;
        case ui::DashboardAction::select_incident:
            incident_viewer_service_->request_detail(command.incident_id);
            break;
        case ui::DashboardAction::select_incident_process:
            incident_viewer_service_->request_process(command.incident_id,
                                                      command.process_identity);
            break;
        case ui::DashboardAction::save_incident_annotation:
        case ui::DashboardAction::save_incident_feedback:
            incident_viewer_service_->update_annotation(
                command.incident_id, command.label, command.note,
                static_cast<storage::IncidentUserFeedback>(command.incident_feedback),
                static_cast<storage::IncidentCategory>(command.incident_category));
            break;
        case ui::DashboardAction::save_contributor_feedback:
            incident_viewer_service_->update_contributor_feedback(
                command.incident_id, command.contributor_executable_key,
                static_cast<storage::ContributorFeedbackResource>(
                    command.contributor_resource),
                static_cast<storage::ContributorFeedbackDisposition>(
                    command.contributor_attribution),
                static_cast<storage::ContributorFeedbackTemporalRelationship>(
                    command.contributor_temporal_relationship));
            break;
        case ui::DashboardAction::refresh_recurring_incidents:
            incident_viewer_service_->request_recurring_incidents();
            break;
        case ui::DashboardAction::save_recurring_group_override:
            incident_viewer_service_->update_recurring_group_override(
                command.incident_id, command.recurring_group_override);
            break;
        case ui::DashboardAction::reset_feedback_profile:
            incident_viewer_service_->reset_feedback_profile();
            break;
        case ui::DashboardAction::rollback_feedback_profile_reset:
            incident_viewer_service_->rollback_feedback_profile_reset();
            break;
        case ui::DashboardAction::refresh_archive_health:
            archive_maintenance_service_->refresh();
            break;
        case ui::DashboardAction::retry_failed_incident:
            archive_maintenance_service_->retry_failed();
            break;
        case ui::DashboardAction::backup_archive:
            archive_maintenance_service_->backup(command.backup_path);
            break;
        case ui::DashboardAction::restore_archive:
            archive_maintenance_service_->restore(command.restore_path,
                                                   command.safety_backup_path);
            break;
        case ui::DashboardAction::retain_incidents:
            archive_maintenance_service_->retain_newest(command.retention_incidents);
            break;
        case ui::DashboardAction::export_dataset:
            archive_maintenance_service_->export_dataset(command.export_path);
            break;
        case ui::DashboardAction::export_failed_incident:
            archive_maintenance_service_->export_failed(command.failed_export_path);
            break;
        case ui::DashboardAction::purge_archive:
            archive_maintenance_service_->purge_all();
            break;
#else
        case ui::DashboardAction::refresh_incidents:
        case ui::DashboardAction::select_incident:
        case ui::DashboardAction::select_incident_process:
        case ui::DashboardAction::save_incident_annotation:
        case ui::DashboardAction::save_incident_feedback:
        case ui::DashboardAction::save_contributor_feedback:
        case ui::DashboardAction::refresh_recurring_incidents:
        case ui::DashboardAction::save_recurring_group_override:
        case ui::DashboardAction::reset_feedback_profile:
        case ui::DashboardAction::rollback_feedback_profile_reset:
        case ui::DashboardAction::refresh_archive_health:
        case ui::DashboardAction::retry_failed_incident:
        case ui::DashboardAction::backup_archive:
        case ui::DashboardAction::restore_archive:
        case ui::DashboardAction::retain_incidents:
        case ui::DashboardAction::export_dataset:
        case ui::DashboardAction::export_failed_incident:
        case ui::DashboardAction::purge_archive:
            break;
#endif
        }

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer_, 18, 20, 24, 255);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_RenderPresent(renderer_);
    }

    shutdown();
    if (diagnostic_started_ && (!diagnostic_completed_ || diagnostic_report_failed_)) {
        return 3;
    }
    return 0;
}

void Application::synchronize_product_ui() noexcept {
    const auto copy = [](auto& destination, const std::string& source) {
        destination.fill('\0');
        const auto count = std::min(source.size(), destination.size() - 1U);
        std::memcpy(destination.data(), source.data(), count);
    };
    product_ui_state_.onboarding_open = !product_settings_.onboarding_completed;
    product_ui_state_.settings_initialized = true;
    product_ui_state_.hotkey_key =
        static_cast<std::uint32_t>(product_settings_.incident_hotkey.key);
    product_ui_state_.hotkey_control = product_settings_.incident_hotkey.control;
    product_ui_state_.hotkey_shift = product_settings_.incident_hotkey.shift;
    product_ui_state_.hotkey_alt = product_settings_.incident_hotkey.alt;
    product_ui_state_.hotkey_windows = product_settings_.incident_hotkey.windows;
    product_ui_state_.automatic_detection =
        product_settings_.automatic_detection_enabled;
    product_ui_state_.detector_sensitivity =
        static_cast<int>(product_settings_.detector_sensitivity);
    product_ui_state_.detect_cpu = product_settings_.detect_cpu;
    product_ui_state_.detect_memory = product_settings_.detect_memory;
    product_ui_state_.detect_disk = product_settings_.detect_disk;
    product_ui_state_.detect_network = product_settings_.detect_network;
    product_ui_state_.detector_cooldown_seconds =
        static_cast<std::uint64_t>(product_settings_.detector_cooldown.count());
    product_ui_state_.notifications = product_settings_.notifications_enabled;
    product_ui_state_.record_foreground_application =
        product_settings_.record_foreground_application;
    product_ui_state_.record_process_lifecycle =
        product_settings_.record_process_lifecycle;
    product_ui_state_.record_power_and_device_events =
        product_settings_.record_power_and_device_events;
    product_ui_state_.record_audio_device_events =
        product_settings_.record_audio_device_events;
    product_ui_state_.record_windows_event_log_evidence =
        product_settings_.record_windows_event_log_evidence;
    product_ui_state_.archive_maximum_mib =
        product_settings_.archive_maximum_bytes >> 20U;
    copy(product_ui_state_.archive_path, product_settings_.archive_path.string());
    const auto parent = product_settings_.archive_path.parent_path();
    copy(product_ui_state_.backup_path,
         (parent / "incidents-backup.sqlite3").string());
    copy(product_ui_state_.safety_backup_path,
         (parent / "incidents-pre-restore.sqlite3").string());
    copy(product_ui_state_.export_path,
         (parent / "blackbox-evidence-dataset").string());
    copy(product_ui_state_.failed_export_path,
         (parent / "recoverable-incident.sqlite3").string());
    if (product_ui_state_.support_bundle_path.front() == '\0') {
        copy(product_ui_state_.support_bundle_path,
             (parent / "blackbox-support-bundle").string());
    }
}

void Application::request_support_bundle(
    const ui::DashboardCommand& command) noexcept {
    try {
        SupportBundleRequest request{};
        request.destination = command.support_bundle_path;
        auto& value = request.diagnostics;
        value.application_version = std::string{core::version};
        value.platform = BLACKBOX_PLATFORM_NAME;
        value.collector_running = dashboard_state_.recorder_status == "Recording";
        value.automatic_detection_enabled =
            dashboard_state_.automatic_detection_enabled;
        value.process_path_collection_enabled =
            product_ui_state_.collect_process_paths;
        value.foreground_identity_enabled =
            product_settings_.record_foreground_application;
        value.process_lifecycle_enabled =
            product_settings_.record_process_lifecycle;
        value.power_and_device_events_enabled =
            product_settings_.record_power_and_device_events;
        value.audio_device_events_enabled =
            product_settings_.record_audio_device_events;
        value.windows_event_evidence_enabled =
            product_settings_.record_windows_event_log_evidence;
        value.collections = dashboard_state_.collection_count;
        value.partial_samples = dashboard_state_.partial_samples;
        value.failed_samples = dashboard_state_.failed_samples;
        value.dropped_samples = dashboard_state_.dropped_samples;
        value.deadline_misses = dashboard_state_.deadline_misses;
        value.resume_events = dashboard_state_.resume_events;
        value.provider_recoveries = dashboard_state_.provider_recoveries;
        value.collector_worker_failures = dashboard_state_.collector_worker_failures;
        value.incident_captures_started = dashboard_state_.incident_captures_started;
        value.incidents_completed = dashboard_state_.incidents_completed;
        value.incident_snapshot_failures =
            dashboard_state_.incident_snapshot_failures;
        value.incident_queue_rejections = dashboard_state_.incident_queue_rejections;
        value.automatic_detector_triggers =
            dashboard_state_.automatic_detector_triggers;
        value.system_events_recorded = dashboard_state_.system_events_recorded;
        value.system_events_dropped = dashboard_state_.system_events_dropped;
        value.process_lifecycle_observations =
            dashboard_state_.process_lifecycle_observations;
        value.process_lifecycle_events_recorded =
            dashboard_state_.process_lifecycle_events_recorded;
#if BLACKBOX_STORAGE_ENABLED
        value.storage_enabled = true;
#endif
        value.storage_writer_running = dashboard_state_.storage_writer_running;
        value.archive_healthy = dashboard_state_.archive_healthy;
        value.archive_schema_version = dashboard_state_.archive_schema_version;
        value.stored_incidents = dashboard_state_.stored_incident_count;
        value.archive_database_bytes = dashboard_state_.archive_database_size_bytes;
        value.archive_maximum_bytes = dashboard_state_.archive_maximum_bytes;
        value.storage_write_attempts = dashboard_state_.storage_write_attempts;
        value.storage_retry_attempts = dashboard_state_.storage_retry_attempts;
        value.storage_retry_exhausted = dashboard_state_.storage_retry_exhausted;
        value.storage_write_successes = dashboard_state_.storage_write_successes;
        value.storage_write_failures = dashboard_state_.storage_write_failures;
        value.recoverable_incident_available =
            dashboard_state_.archive_recoverable_incident;
        value.previous_crash_dumps = dashboard_state_.previous_crash_dumps;
        if (command.include_latest_crash_dump && crash_diagnostics_ != nullptr) {
            const auto crash = crash_diagnostics_->snapshot();
            if (!crash.latest_dump.empty()) {
                request.consented_crash_dump = crash.latest_dump;
                request.crash_dump_disclosure_confirmed =
                    command.crash_dump_disclosure_confirmed;
            }
        }
        support_bundle_service_.create(std::move(request));
    } catch (...) {
        core::Logger::write(core::LogLevel::error,
                            "Support bundle request could not be prepared");
    }
}

bool Application::register_configured_hotkey(
    const platform::HotkeyCombination combination) noexcept {
    if (hotkey_manager_ == nullptr) return false;
    const auto result = hotkey_manager_->register_hotkey(
        combination, [this] { request_incident_capture(); });
    std::string name;
    if (combination.control) name += "Ctrl+";
    if (combination.shift) name += "Shift+";
    if (combination.alt) name += "Alt+";
    if (combination.windows) name += "Win+";
    name += "F" + std::to_string(static_cast<unsigned>(combination.key));
    switch (result) {
    case platform::HotkeyRegistrationResult::registered:
        hotkey_status_ = name + " registered";
        return true;
    case platform::HotkeyRegistrationResult::conflict:
        hotkey_status_ = name + " is already in use";
        break;
    case platform::HotkeyRegistrationResult::unavailable:
        hotkey_status_ = "Hotkey registration unavailable";
        break;
    case platform::HotkeyRegistrationResult::invalid_combination:
        hotkey_status_ = "Invalid hotkey configuration";
        break;
    }
    return false;
}

void Application::apply_product_settings(
    const ui::DashboardCommand& command) noexcept {
    auto proposed = product_settings_;
    proposed.incident_hotkey = {
        static_cast<platform::HotkeyKey>(command.hotkey_key),
        command.hotkey_control, command.hotkey_shift, command.hotkey_alt,
        command.hotkey_windows};
    proposed.automatic_detection_enabled = command.automatic_detection;
    proposed.detector_sensitivity =
        static_cast<DetectorSensitivity>(command.detector_sensitivity);
    proposed.detect_cpu = command.detect_cpu;
    proposed.detect_memory = command.detect_memory;
    proposed.detect_disk = command.detect_disk;
    proposed.detect_network = command.detect_network;
    proposed.detector_cooldown =
        std::chrono::seconds{command.detector_cooldown_seconds};
    proposed.notifications_enabled = command.notifications;
    proposed.record_foreground_application = command.record_foreground_application;
    proposed.record_process_lifecycle = command.record_process_lifecycle;
    proposed.record_power_and_device_events = command.record_power_and_device_events;
    proposed.record_audio_device_events = command.record_audio_device_events;
    proposed.record_windows_event_log_evidence =
        command.record_windows_event_log_evidence;
    proposed.archive_path = command.archive_path;
    if (command.archive_maximum_mib > (maximum_archive_bytes >> 20U)) {
        recorder_settings_status_text_ = "Rejected: archive capacity exceeds 64 GiB";
        return;
    }
    proposed.archive_maximum_bytes = command.archive_maximum_mib << 20U;
    const auto validated_product = validate_product_settings(proposed);
    if (!validated_product) {
        recorder_settings_status_text_ = "Rejected: " +
                                         validated_product.error().message;
        return;
    }

    auto recorder = collector_->diagnostics().configuration;
    recorder.incident_pre_window =
        std::chrono::seconds{command.incident_pre_window_seconds};
    recorder.incident_post_window =
        std::chrono::seconds{command.incident_post_window_seconds};
    recorder.collect_process_paths = command.collect_process_paths;
    const auto validated_recorder = telemetry::validate_recorder_configuration(recorder);
    if (!validated_recorder) {
        recorder_settings_status_text_ = "Rejected: capture window or privacy settings exceed recorder bounds";
        return;
    }

#if defined(_WIN32)
    if (proposed.incident_hotkey != product_settings_.incident_hotkey) {
        hotkey_manager_->unregister_hotkey();
        if (!register_configured_hotkey(proposed.incident_hotkey)) {
            const auto failure = hotkey_status_;
            static_cast<void>(register_configured_hotkey(
                product_settings_.incident_hotkey));
            recorder_settings_status_text_ = "Rejected: " + failure +
                                             "; previous hotkey restored";
            return;
        }
    }
#endif

    const bool restart = collector_->running();
    if (system_event_collector_ != nullptr) system_event_collector_->stop();
    collector_->stop();
#if BLACKBOX_AUTOMATIC_DETECTION_ENABLED
    if (const auto reconfigured = automatic_detector_->reconfigure(
            detector_configuration(*validated_product)); !reconfigured) {
        if (restart) collector_->start();
        recorder_settings_status_text_ = "Rejected: detector configuration is invalid";
        return;
    }
#endif
    collector_->reconfigure(*validated_recorder);
    collector_->set_foreground_application_enabled(
        validated_product->record_foreground_application);
    collector_->set_process_lifecycle_enabled(
        validated_product->record_process_lifecycle);
    if (system_event_collector_ != nullptr) {
        auto event_configuration = telemetry::EventCollectorConfiguration{};
        event_configuration.provider = event_provider_configuration(*validated_product);
        event_configuration.automatic_system_event_capture =
            validated_product->automatic_detection_enabled;
        system_event_collector_->reconfigure(event_configuration);
    }
    collector_->set_automatic_detection_enabled(
        validated_product->automatic_detection_enabled);
    if (restart) {
        collector_->start();
        if (system_event_collector_ != nullptr && any_event_source_enabled(
                event_provider_configuration(*validated_product))) {
            system_event_collector_->start();
        }
    }
    if (background_shell_ != nullptr) {
        background_shell_->set_notifications_enabled(
            validated_product->notifications_enabled);
    }
    product_settings_ = *validated_product;
    product_ui_state_.collect_process_paths = recorder.collect_process_paths;
    product_ui_state_.incident_pre_window_seconds =
        command.incident_pre_window_seconds;
    product_ui_state_.incident_post_window_seconds =
        command.incident_post_window_seconds;

    const auto saved_product = save_product_settings(product_settings_path_,
                                                     product_settings_);
    const auto saved_recorder = save_recorder_settings(recorder_settings_path_,
                                                       *validated_recorder);
    if (!saved_product || !saved_recorder) {
        recorder_settings_status_text_ = "Applied for this session; one or more settings files could not be saved";
    } else {
        recorder_settings_status_text_ =
            "Applied and saved. Archive path/capacity changes take effect next launch.";
    }
    dashboard_state_.hotkey_status = hotkey_status_;
    next_dashboard_refresh_at_ = telemetry_clock_.now();
}

void Application::refresh_dashboard_if_due() {
    const auto now = telemetry_clock_.now();
    if (collector_ == nullptr || now < next_dashboard_refresh_at_) {
        return;
    }
    do {
        next_dashboard_refresh_at_ += 250ms;
    } while (next_dashboard_refresh_at_ <= now);

    refresh_accessibility_if_due();
    const auto diagnostics = collector_->diagnostics();
    dashboard_state_.display_scale = SDL_GetWindowDisplayScale(window_);
    const auto snapshot = collector_->snapshot(ui::dashboard_history_capacity);
    auto active_processes = collector_->active_process_snapshot();
    dashboard_state_.recorder_status = diagnostics.running ? "Recording" : "Stopped";
    dashboard_state_.provider_status = provider_status_text(diagnostics.provider_status);
    dashboard_state_.collection_count = diagnostics.collection_count;
    dashboard_state_.partial_samples = diagnostics.partial_samples;
    dashboard_state_.failed_samples = diagnostics.failed_samples;
    dashboard_state_.dropped_samples = diagnostics.dropped_samples;
    dashboard_state_.late_samples = diagnostics.late_samples;
    dashboard_state_.deadline_misses = diagnostics.deadline_misses;
    dashboard_state_.resume_events = diagnostics.resume_events;
    dashboard_state_.resume_skipped_samples = diagnostics.resume_skipped_samples;
    dashboard_state_.last_resume_gap_seconds = seconds(diagnostics.last_resume_gap);
    dashboard_state_.provider_recoveries = diagnostics.provider_recoveries;
    dashboard_state_.consecutive_provider_failures =
        diagnostics.consecutive_provider_failures;
    dashboard_state_.collector_worker_failures = diagnostics.worker_failures;
    dashboard_state_.ring_size = diagnostics.ring.size;
    dashboard_state_.ring_capacity = diagnostics.ring.capacity;
    dashboard_state_.ring_overwrites = diagnostics.ring.overwritten_samples;
    dashboard_state_.ring_utilization = diagnostics.ring.utilization();
    dashboard_state_.sample_interval_milliseconds = milliseconds(
        diagnostics.configuration.sample_interval);
    dashboard_state_.history_duration_seconds = seconds(
        diagnostics.configuration.history_duration);
    dashboard_state_.timing_samples = diagnostics.collection_timing.samples_recorded;
    dashboard_state_.timing_average_microseconds = microseconds(
        diagnostics.collection_timing.average);
    dashboard_state_.timing_p50_microseconds = microseconds(
        diagnostics.collection_timing.p50);
    dashboard_state_.timing_p95_microseconds = microseconds(
        diagnostics.collection_timing.p95);
    dashboard_state_.timing_p99_microseconds = microseconds(
        diagnostics.collection_timing.p99);
    dashboard_state_.timing_maximum_microseconds = microseconds(
        diagnostics.collection_timing.maximum);
    dashboard_state_.jitter_average_microseconds = microseconds(
        diagnostics.scheduling_jitter.average);
    dashboard_state_.jitter_p50_microseconds = microseconds(
        diagnostics.scheduling_jitter.p50);
    dashboard_state_.jitter_p95_microseconds = microseconds(
        diagnostics.scheduling_jitter.p95);
    dashboard_state_.jitter_p99_microseconds = microseconds(
        diagnostics.scheduling_jitter.p99);
    dashboard_state_.jitter_maximum_microseconds = microseconds(
        diagnostics.scheduling_jitter.maximum);
    dashboard_state_.active_process_count = diagnostics.active_processes;
    dashboard_state_.process_metadata_count = diagnostics.process_metadata_entries;
    dashboard_state_.process_metadata_capacity = diagnostics.process_metadata_capacity;
    dashboard_state_.process_metadata_evictions = diagnostics.process_metadata_evictions;
    dashboard_state_.process_inaccessible = diagnostics.process_inaccessible;
    dashboard_state_.process_exits_during_sampling =
        diagnostics.processes_exited_during_sample;
    dashboard_state_.process_samples_truncated = diagnostics.process_samples_truncated;
    dashboard_state_.process_lifecycle_observations =
        diagnostics.process_lifecycle_observations;
    dashboard_state_.process_lifecycle_events_recorded =
        diagnostics.process_lifecycle_events_recorded;
    if (system_event_collector_ != nullptr) {
        const auto events = system_event_collector_->diagnostics();
        dashboard_state_.event_collector_running = events.running;
        dashboard_state_.system_events_recorded = events.events_recorded;
        dashboard_state_.system_events_dropped =
            events.native_events_dropped + events.ring.overwritten_samples;
        dashboard_state_.system_event_ring_size = events.ring.size;
        dashboard_state_.system_event_ring_capacity = events.ring.capacity;
        dashboard_state_.automatic_event_capture_requests =
            events.automatic_event_requests;
        dashboard_state_.automatic_event_capture_rejections =
            events.automatic_event_capture_rejections;
        if (!product_settings_.automatic_detection_enabled) {
            dashboard_state_.automatic_event_capture_status =
                "Disabled with automatic incident detection";
        } else if (!event_provider_configuration(product_settings_).application_events ||
                   !event_provider_configuration(product_settings_).display_driver_events ||
                   !event_provider_configuration(product_settings_).storage_events) {
            dashboard_state_.automatic_event_capture_status =
                "Disabled: Windows Event Log evidence recording is off";
        } else if (events.capabilities.application_events &&
                   events.capabilities.display_driver_events &&
                   events.capabilities.storage_events) {
            dashboard_state_.automatic_event_capture_status =
                "Supported: Application crash 1000, Hang 1002, Display recovery 4101, and Disk retry 153";
        } else {
            dashboard_state_.automatic_event_capture_status =
                "Partially unavailable: a Windows symptom subscription failed";
        }
    }
    dashboard_state_.hotkey_status = hotkey_status_;
    dashboard_state_.recorder_settings_status = recorder_settings_status_text_;
    dashboard_state_.incident_capture_status = capture_phase_text(
        diagnostics.incident_capture.phase);
    dashboard_state_.incident_capture_enabled = diagnostics.incident_capture.can_request;
    dashboard_state_.incident_pre_window_seconds = seconds(
        diagnostics.configuration.incident_pre_window);
    dashboard_state_.incident_post_window_seconds = seconds(
        diagnostics.configuration.incident_post_window);
    dashboard_state_.incident_post_remaining_seconds = 0.0;
    if (diagnostics.incident_capture.has_pending_window &&
        diagnostics.incident_capture.pending_window.requested_end > now) {
        dashboard_state_.incident_post_remaining_seconds = seconds(
            diagnostics.incident_capture.pending_window.requested_end - now);
    }
    dashboard_state_.incident_queue_size = diagnostics.incident_capture.queue_size;
    dashboard_state_.incident_queue_capacity = diagnostics.incident_capture.queue_capacity;
    dashboard_state_.incident_captures_started =
        diagnostics.incident_capture.captures_started;
    dashboard_state_.incident_requests_merged =
        diagnostics.incident_capture.capture_requests_merged;
    dashboard_state_.incidents_completed =
        diagnostics.incident_capture.incidents_completed;
    dashboard_state_.incident_queue_rejections =
        diagnostics.incident_capture.queue_rejections;
    dashboard_state_.incident_snapshot_failures =
        diagnostics.incident_capture.snapshot_failures;
    dashboard_state_.incident_captures_cancelled =
        diagnostics.incident_capture.captures_cancelled;
    dashboard_state_.automatic_detection_enabled =
        diagnostics.automatic_detection_enabled;
    dashboard_state_.automatic_detector_samples =
        diagnostics.automatic_detector.samples_observed;
    dashboard_state_.automatic_detector_triggers =
        diagnostics.automatic_detector.triggers_emitted;
    dashboard_state_.automatic_detector_cooldown_suppressions =
        diagnostics.automatic_detector.triggers_suppressed_by_cooldown;
    dashboard_state_.automatic_detector_single_observation_triggers =
        diagnostics.automatic_detector.single_observation_triggers;
    dashboard_state_.automatic_capture_rejections =
        diagnostics.automatic_capture_rejections +
        dashboard_state_.automatic_event_capture_rejections;
    dashboard_state_.incident_snapshot_average_microseconds = microseconds(
        diagnostics.incident_snapshot_timing.average);
    dashboard_state_.incident_snapshot_p95_microseconds = microseconds(
        diagnostics.incident_snapshot_timing.p95);
    dashboard_state_.incident_snapshot_p99_microseconds = microseconds(
        diagnostics.incident_snapshot_timing.p99);
    dashboard_state_.incident_snapshot_maximum_microseconds = microseconds(
        diagnostics.incident_snapshot_timing.maximum);
#if BLACKBOX_STORAGE_ENABLED
    if (incident_writer_ != nullptr) {
        const auto writer = incident_writer_->diagnostics();
        if (writer.retrying) {
            storage_status_text_ = "Retrying incident persistence: " +
                                   writer.last_error_message;
        } else if (writer.state == storage::WriterState::degraded) {
            storage_status_text_ = "Degraded: " + writer.last_error_message;
        } else if (writer.state == storage::WriterState::running) {
            storage_status_text_ = "Ready";
        }
        dashboard_state_.storage_writer_running =
            writer.state == storage::WriterState::running ||
            writer.state == storage::WriterState::degraded;
        dashboard_state_.storage_writing = writer.writing;
        dashboard_state_.storage_retrying = writer.retrying;
        dashboard_state_.storage_write_attempts = writer.attempts;
        dashboard_state_.storage_retry_attempts = writer.retry_attempts;
        dashboard_state_.storage_retry_exhausted = writer.retry_exhausted;
        dashboard_state_.storage_write_successes = writer.succeeded;
        dashboard_state_.storage_write_failures = writer.failed;
        dashboard_state_.storage_write_cancellations = writer.cancelled;
        dashboard_state_.stored_incident_count =
            stored_incidents_at_start_ + writer.succeeded;
        dashboard_state_.storage_write_average_microseconds = microseconds(
            writer.write_timing.average);
        dashboard_state_.storage_write_p95_microseconds = microseconds(
            writer.write_timing.p95);
        dashboard_state_.storage_write_p99_microseconds = microseconds(
            writer.write_timing.p99);
        dashboard_state_.storage_write_maximum_microseconds = microseconds(
            writer.write_timing.maximum);
        if (writer.succeeded != viewer_last_writer_successes_) {
            viewer_last_writer_successes_ = writer.succeeded;
            if (archive_maintenance_service_ != nullptr) {
                archive_maintenance_service_->refresh();
            }
            if (incident_viewer_service_ != nullptr) {
                incident_viewer_service_->request_page(
                    0U, incident_viewer_state_.search.data(), incident_viewer_state_.order);
                incident_viewer_service_->request_recurring_incidents();
            }
        }
    }
    if (incident_viewer_service_ != nullptr) {
        incident_viewer_state_.content = incident_viewer_service_->snapshot();
    }
    if (archive_maintenance_service_ != nullptr) {
        const auto maintenance = archive_maintenance_service_->snapshot();
        dashboard_state_.archive_maintenance_busy = maintenance->busy;
        dashboard_state_.archive_healthy = maintenance->healthy;
        dashboard_state_.archive_recoverable_incident =
            maintenance->recoverable_incident;
        dashboard_state_.archive_recoverable_sequence =
            maintenance->recoverable_capture_sequence;
        dashboard_state_.archive_database_size_bytes =
            maintenance->database_size_bytes;
        dashboard_state_.archive_maximum_bytes = maintenance->maximum_bytes;
        dashboard_state_.archive_schema_version = maintenance->schema_version;
        dashboard_state_.archive_path = maintenance->archive_path;
        dashboard_state_.archive_maintenance_status = maintenance->status;
        if (!maintenance->busy && maintenance->incident_count !=
            dashboard_state_.stored_incident_count) {
            dashboard_state_.stored_incident_count = maintenance->incident_count;
        }
        if (!maintenance->busy && maintenance->generation !=
            archive_maintenance_generation_) {
            archive_maintenance_generation_ = maintenance->generation;
            if (incident_viewer_service_ != nullptr) {
                incident_viewer_service_->request_page(
                    0U, incident_viewer_state_.search.data(),
                    incident_viewer_state_.order);
                incident_viewer_service_->request_recurring_incidents();
            }
        }
    }
#endif
    dashboard_state_.storage_status = storage_status_text_;

    const auto support = support_bundle_service_.snapshot();
    dashboard_state_.support_bundle_busy = support->busy;
    dashboard_state_.support_bundle_status = support->status;
    if (crash_diagnostics_ != nullptr) {
        const auto crash = crash_diagnostics_->snapshot();
        dashboard_state_.crash_diagnostics_available = crash.available;
        dashboard_state_.crash_diagnostics_armed = crash.armed;
        dashboard_state_.previous_crash_dumps = crash.completed_dumps;
        dashboard_state_.latest_crash_dump_available = !crash.latest_dump.empty();
        dashboard_state_.crash_diagnostics_status = crash.status;
    }

    std::sort(active_processes.frame.processes.begin(),
              active_processes.frame.processes.end(),
              [](const telemetry::ProcessSample& left,
                 const telemetry::ProcessSample& right) {
                  const auto left_cpu = left.cpu_usage.has_value()
                                            ? left.cpu_usage.value.value
                                            : -1.0;
                  const auto right_cpu = right.cpu_usage.has_value()
                                             ? right.cpu_usage.value.value
                                             : -1.0;
                  if (left_cpu != right_cpu) {
                      return left_cpu > right_cpu;
                  }
                  const auto left_memory = left.working_set.has_value()
                                               ? left.working_set.value.value
                                               : 0U;
                  const auto right_memory = right.working_set.has_value()
                                                ? right.working_set.value.value
                                                : 0U;
                  return left_memory > right_memory;
              });
    dashboard_state_.process_count = std::min(
        active_processes.frame.processes.size(), ui::dashboard_process_capacity);
    for (std::size_t index = 0U; index < dashboard_state_.process_count; ++index) {
        const auto& process = active_processes.frame.processes[index];
        auto& row = dashboard_state_.processes[index];
        row.name.clear();
        row.executable_path.clear();
        row.pid = process.identity.pid.value;
        row.cpu_status = display_status(process.cpu_usage.status);
        row.memory_status = display_status(process.working_set.status);
        row.disk_read_status = display_status(process.disk_read_rate.status);
        row.disk_write_status = display_status(process.disk_write_rate.status);
        row.cpu_percent = 0.0;
        row.working_set_mib = 0.0;
        row.disk_read_mib_per_second = 0.0;
        row.disk_write_mib_per_second = 0.0;
        if (process.cpu_usage.has_value()) {
            row.cpu_percent = process.cpu_usage.value.value * 100.0;
        }
        if (process.working_set.has_value()) {
            row.working_set_mib = static_cast<double>(
                process.working_set.value.value) / (1024.0 * 1024.0);
        }
        if (process.disk_read_rate.has_value()) {
            row.disk_read_mib_per_second = mebibytes_per_second(
                process.disk_read_rate.value);
        }
        if (process.disk_write_rate.has_value()) {
            row.disk_write_mib_per_second = mebibytes_per_second(
                process.disk_write_rate.value);
        }
        const auto metadata = std::find_if(
            active_processes.metadata.begin(), active_processes.metadata.end(),
            [&process](const telemetry::ProcessInfo& info) {
                return info.identity == process.identity;
            });
        if (metadata != active_processes.metadata.end()) {
            row.name = metadata->name.has_value()
                           ? metadata->name.value
                           : "<name unavailable>";
            if (metadata->executable_path.has_value()) {
                row.executable_path = metadata->executable_path.value;
            }
        } else {
            row.name = "<metadata pending>";
        }
    }

    const auto unavailable = std::numeric_limits<float>::quiet_NaN();
    dashboard_state_.cpu_history.fill(unavailable);
    dashboard_state_.memory_history.fill(unavailable);
    dashboard_state_.disk_read_history.fill(unavailable);
    dashboard_state_.disk_write_history.fill(unavailable);
    dashboard_state_.network_receive_history.fill(unavailable);
    dashboard_state_.network_transmit_history.fill(unavailable);
    dashboard_state_.history_size = snapshot.size();
    dashboard_state_.cpu_history_points = 0U;
    dashboard_state_.memory_history_points = 0U;
    dashboard_state_.disk_read_history_points = 0U;
    dashboard_state_.disk_write_history_points = 0U;
    dashboard_state_.network_receive_history_points = 0U;
    dashboard_state_.network_transmit_history_points = 0U;
    dashboard_state_.disk_history_max_mib_per_second = 1.0;
    dashboard_state_.network_history_max_mib_per_second = 1.0;
    for (std::size_t index = 0U; index < snapshot.size(); ++index) {
        const auto& sample = snapshot.samples()[index];
        if (sample.cpu_usage.has_value()) {
            const auto point = dashboard_state_.cpu_history_points++;
            dashboard_state_.cpu_history_x[point] = static_cast<float>(index);
            dashboard_state_.cpu_history[point] = static_cast<float>(
                sample.cpu_usage.value.value * 100.0);
        }
        if (sample.memory_usage.has_value()) {
            const auto point = dashboard_state_.memory_history_points++;
            dashboard_state_.memory_history_x[point] = static_cast<float>(index);
            dashboard_state_.memory_history[point] = static_cast<float>(
                sample.memory_usage.value.value * 100.0);
        }
        if (sample.disk_read_rate.has_value()) {
            const auto point = dashboard_state_.disk_read_history_points++;
            const auto value = mebibytes_per_second(sample.disk_read_rate.value);
            dashboard_state_.disk_read_history_x[point] = static_cast<float>(index);
            dashboard_state_.disk_read_history[point] = static_cast<float>(value);
            dashboard_state_.disk_history_max_mib_per_second = std::max(
                dashboard_state_.disk_history_max_mib_per_second, value);
        }
        if (sample.disk_write_rate.has_value()) {
            const auto point = dashboard_state_.disk_write_history_points++;
            const auto value = mebibytes_per_second(sample.disk_write_rate.value);
            dashboard_state_.disk_write_history_x[point] = static_cast<float>(index);
            dashboard_state_.disk_write_history[point] = static_cast<float>(value);
            dashboard_state_.disk_history_max_mib_per_second = std::max(
                dashboard_state_.disk_history_max_mib_per_second, value);
        }
        if (sample.network_receive_rate.has_value()) {
            const auto point = dashboard_state_.network_receive_history_points++;
            const auto value = mebibytes_per_second(sample.network_receive_rate.value);
            dashboard_state_.network_receive_history_x[point] = static_cast<float>(index);
            dashboard_state_.network_receive_history[point] = static_cast<float>(value);
            dashboard_state_.network_history_max_mib_per_second = std::max(
                dashboard_state_.network_history_max_mib_per_second, value);
        }
        if (sample.network_transmit_rate.has_value()) {
            const auto point = dashboard_state_.network_transmit_history_points++;
            const auto value = mebibytes_per_second(sample.network_transmit_rate.value);
            dashboard_state_.network_transmit_history_x[point] = static_cast<float>(index);
            dashboard_state_.network_transmit_history[point] = static_cast<float>(value);
            dashboard_state_.network_history_max_mib_per_second = std::max(
                dashboard_state_.network_history_max_mib_per_second, value);
        }
    }

    if (!snapshot.empty()) {
        const auto& latest = snapshot.samples().back();
        dashboard_state_.cpu_status = display_status(latest.cpu_usage.status);
        if (latest.cpu_usage.has_value()) {
            dashboard_state_.cpu_usage = latest.cpu_usage.value.value;
        }
        dashboard_state_.memory_status = display_status(latest.memory_usage.status);
        if (latest.memory_usage.has_value() && latest.memory_used.has_value() &&
            latest.memory_total.has_value()) {
            dashboard_state_.memory_usage = latest.memory_usage.value.value;
            dashboard_state_.memory_used_bytes = latest.memory_used.value.value;
            dashboard_state_.memory_total_bytes = latest.memory_total.value.value;
        }
        dashboard_state_.disk_read_status = display_status(latest.disk_read_rate.status);
        dashboard_state_.disk_write_status = display_status(latest.disk_write_rate.status);
        dashboard_state_.network_receive_status = display_status(
            latest.network_receive_rate.status);
        dashboard_state_.network_transmit_status = display_status(
            latest.network_transmit_rate.status);
        dashboard_state_.disk_latency_status = display_status(
            latest.disk_service_time.status);
        dashboard_state_.disk_queue_status = display_status(
            latest.disk_queue_depth.status);
        dashboard_state_.network_connectivity_status = display_status(
            latest.network_connectivity.status);
        dashboard_state_.network_transport_quality_status = display_status(
            latest.network_tcp_retransmit_fraction.status);
        dashboard_state_.gpu_status = display_status(latest.gpu_usage.status);
        dashboard_state_.gpu_memory_status = display_status(
            latest.gpu_dedicated_memory.status);
        dashboard_state_.foreground_status = display_status(
            latest.foreground_process.status);
        dashboard_state_.dpc_status = display_status(latest.dpc_usage.status);
        dashboard_state_.cpu_frequency_status = display_status(
            latest.cpu_current_mhz.status);
        dashboard_state_.cpu_thermal_limit_status = display_status(
            latest.cpu_thermal_limit_fraction.status);
        dashboard_state_.power_status = display_status(latest.power_source.status);
        if (latest.disk_read_rate.has_value()) {
            dashboard_state_.disk_read_mib_per_second = mebibytes_per_second(
                latest.disk_read_rate.value);
        }
        if (latest.disk_write_rate.has_value()) {
            dashboard_state_.disk_write_mib_per_second = mebibytes_per_second(
                latest.disk_write_rate.value);
        }
        if (latest.network_receive_rate.has_value()) {
            dashboard_state_.network_receive_mib_per_second = mebibytes_per_second(
                latest.network_receive_rate.value);
        }
        if (latest.network_transmit_rate.has_value()) {
            dashboard_state_.network_transmit_mib_per_second = mebibytes_per_second(
                latest.network_transmit_rate.value);
        }
        if (latest.disk_read_latency.has_value()) {
            dashboard_state_.disk_read_latency_milliseconds =
                latest.disk_read_latency.value.value * 1'000.0;
        }
        if (latest.disk_write_latency.has_value()) {
            dashboard_state_.disk_write_latency_milliseconds =
                latest.disk_write_latency.value.value * 1'000.0;
        }
        if (latest.disk_service_time.has_value()) {
            dashboard_state_.disk_service_time_milliseconds =
                latest.disk_service_time.value.value * 1'000.0;
        }
        if (latest.disk_queue_depth.has_value()) {
            dashboard_state_.disk_queue_depth = latest.disk_queue_depth.value;
        }
        if (latest.disk_worst_device_id.has_value()) {
            dashboard_state_.disk_worst_device_id =
                latest.disk_worst_device_id.value;
        }
        if (latest.network_connectivity.has_value()) {
            dashboard_state_.network_connectivity_level = static_cast<std::uint8_t>(
                latest.network_connectivity.value);
        }
        if (latest.network_active_interfaces.has_value()) {
            dashboard_state_.network_active_interfaces =
                latest.network_active_interfaces.value;
        }
        if (latest.network_interface_changes.has_value()) {
            dashboard_state_.network_interface_changes =
                latest.network_interface_changes.value;
        }
        if (latest.network_tcp_retransmit_fraction.has_value()) {
            dashboard_state_.network_tcp_retransmit_percent =
                latest.network_tcp_retransmit_fraction.value.value * 100.0;
        }
        if (latest.network_tcp_failed_connections.has_value()) {
            dashboard_state_.network_tcp_failed_connections =
                latest.network_tcp_failed_connections.value;
        }
        if (latest.network_tcp_resets.has_value()) {
            dashboard_state_.network_tcp_resets = latest.network_tcp_resets.value;
        }
        if (latest.gpu_usage.has_value()) {
            dashboard_state_.gpu_usage = latest.gpu_usage.value.value;
        }
        if (latest.gpu_dedicated_memory.has_value()) {
            dashboard_state_.gpu_dedicated_memory_mib =
                static_cast<double>(latest.gpu_dedicated_memory.value.value) /
                (1024.0 * 1024.0);
        }
        if (latest.gpu_shared_memory.has_value()) {
            dashboard_state_.gpu_shared_memory_mib =
                static_cast<double>(latest.gpu_shared_memory.value.value) /
                (1024.0 * 1024.0);
        }
        if (latest.foreground_process.has_value()) {
            dashboard_state_.foreground_pid = latest.foreground_process.value.pid.value;
        }
        if (latest.foreground_gpu_usage.has_value()) {
            dashboard_state_.foreground_gpu_usage =
                latest.foreground_gpu_usage.value.value;
        }
        if (latest.dpc_usage.has_value()) {
            dashboard_state_.dpc_usage = latest.dpc_usage.value.value;
        }
        if (latest.interrupt_usage.has_value()) {
            dashboard_state_.interrupt_usage = latest.interrupt_usage.value.value;
        }
        if (latest.dpc_rate.has_value()) {
            dashboard_state_.dpc_rate = latest.dpc_rate.value;
        }
        if (latest.cpu_current_mhz.has_value()) {
            dashboard_state_.cpu_current_mhz = latest.cpu_current_mhz.value;
        }
        if (latest.cpu_max_mhz.has_value()) {
            dashboard_state_.cpu_max_mhz = latest.cpu_max_mhz.value;
        }
        if (latest.cpu_thermal_limit_mhz.has_value()) {
            dashboard_state_.cpu_thermal_limit_mhz =
                latest.cpu_thermal_limit_mhz.value;
        }
        if (latest.cpu_thermal_limit_fraction.has_value()) {
            dashboard_state_.cpu_thermal_limit_fraction =
                latest.cpu_thermal_limit_fraction.value.value;
        }
        if (latest.power_source.has_value()) {
            dashboard_state_.power_source = static_cast<std::uint8_t>(
                latest.power_source.value);
        }
        if (latest.battery_fraction.has_value()) {
            dashboard_state_.battery_fraction = latest.battery_fraction.value.value;
        }
        if (latest.battery_saver.has_value()) {
            dashboard_state_.battery_saver = latest.battery_saver.value;
        }
    }
}

void Application::refresh_accessibility_if_due() {
#if defined(_WIN32)
    const auto now = telemetry_clock_.now();
    if (now < next_accessibility_refresh_at_) return;
    do {
        next_accessibility_refresh_at_ += 1s;
    } while (next_accessibility_refresh_at_ <= now);

    const auto accessibility = platform::windows::accessibility_preferences();
    static_cast<void>(ui::update_accessibility_style(
        high_contrast_enabled_, accessibility.high_contrast));
    animations_enabled_ = accessibility.animations_enabled;
    dashboard_state_.accessibility_high_contrast = high_contrast_enabled_;
    dashboard_state_.accessibility_animations_enabled = animations_enabled_;
#endif
}

void Application::refresh_background_shell_if_due() {
    const auto now = telemetry_clock_.now();
    if (!background_shell_started_ || background_shell_ == nullptr ||
        collector_ == nullptr || now < next_background_refresh_at_) {
        return;
    }
    next_background_refresh_at_ = now + 250ms;

    const auto collector_diagnostics = collector_->diagnostics();
    const auto capture_phase = collector_diagnostics.incident_capture.phase;
    const bool capture_active =
        capture_phase == core::IncidentCapturePhase::collecting_post_window ||
        capture_phase == core::IncidentCapturePhase::constructing_snapshot ||
        capture_phase == core::IncidentCapturePhase::queued;
    auto status = recorder_paused_ || !collector_diagnostics.running
                      ? platform::BackgroundShellStatus::paused
                      : capture_active ? platform::BackgroundShellStatus::capturing
                                       : platform::BackgroundShellStatus::recording;

    if (collector_diagnostics.automatic_captures_started >
        background_last_automatic_triggers_) {
        background_last_automatic_triggers_ =
            collector_diagnostics.automatic_captures_started;
        static_cast<void>(background_shell_->notify(
            "BlackBox capture started",
            "An unusual resource pattern triggered an incident capture."));
    }
    if (collector_diagnostics.incident_capture.snapshot_failures >
        background_last_snapshot_failures_) {
        background_last_snapshot_failures_ =
            collector_diagnostics.incident_capture.snapshot_failures;
        static_cast<void>(background_shell_->notify(
            "BlackBox capture failed",
            "The immutable incident snapshot could not be constructed; recording continues."));
    }
    if (collector_diagnostics.incident_capture.captures_cancelled >
        background_last_capture_cancellations_) {
        background_last_capture_cancellations_ =
            collector_diagnostics.incident_capture.captures_cancelled;
        static_cast<void>(background_shell_->notify(
            "BlackBox capture cancelled",
            "The active post-incident window ended before completion."));
    }

#if BLACKBOX_STORAGE_ENABLED
    if (incident_writer_ != nullptr) {
        const auto writer = incident_writer_->diagnostics();
        if (writer.retrying) {
            status = platform::BackgroundShellStatus::retrying_storage;
        } else if (writer.state == storage::WriterState::degraded) {
            status = platform::BackgroundShellStatus::error;
        }
        if (writer.succeeded > background_last_writer_successes_) {
            background_last_writer_successes_ = writer.succeeded;
            static_cast<void>(background_shell_->notify(
                "BlackBox capture completed",
                "The incident was saved to the local archive."));
        }
        if (writer.failed > background_last_writer_failures_) {
            background_last_writer_failures_ = writer.failed;
            const auto detail = writer.last_error_message.empty()
                                    ? std::string{"The local archive rejected the incident."}
                                    : std::string{"The incident could not be saved: "} +
                                          writer.last_error_message;
            static_cast<void>(background_shell_->notify(
                "BlackBox capture failed", detail));
        }
    }
#endif

    background_shell_->set_status(status);
    const auto shell = background_shell_->diagnostics();
    background_status_text_ = shell.tray_available ? "Tray active" : "Tray unavailable";
    background_status_text_ += shell.window_visible ? " | window visible" : " | window hidden";
#if defined(__linux__)
    background_status_text_ += " | notifications unavailable";
#else
    background_status_text_ += shell.notifications_enabled
                                   ? " | notifications on"
                                   : " | notifications quiet";
#endif
    background_status_text_ += background_launch_at_login_enabled_
                                   ? " | starts at login"
                                   : " | manual startup";
    if (shell.tray_readd_failures != 0U) {
        background_status_text_ += " | tray recovery failed";
    }
    dashboard_state_.background_status = background_status_text_;
}

void Application::process_background_commands(bool& running) {
    const auto commands = pending_background_commands_.exchange(
        0U, std::memory_order_acquire);
    const auto requested = [commands](const platform::BackgroundShellCommand command) {
        return (commands & command_bit(command)) != 0U;
    };

    if (requested(platform::BackgroundShellCommand::exit_application)) {
        running = false;
        return;
    }
    if (requested(platform::BackgroundShellCommand::show_window)) {
        show_window();
    }
    if (requested(platform::BackgroundShellCommand::hide_window)) {
        hide_window();
    }
    if (requested(platform::BackgroundShellCommand::capture_incident)) {
        request_incident_capture();
    }
    if (requested(platform::BackgroundShellCommand::toggle_recording) &&
        collector_ != nullptr) {
        if (recorder_paused_) {
            collector_->start();
            if (system_event_collector_ != nullptr && any_event_source_enabled(
                    event_provider_configuration(product_settings_))) {
                system_event_collector_->start();
            }
            recorder_paused_ = false;
            static_cast<void>(background_shell_->notify(
                "BlackBox recording resumed",
                "The bounded rolling recorder is active again."));
        } else {
            if (system_event_collector_ != nullptr) system_event_collector_->stop();
            collector_->stop();
            recorder_paused_ = true;
            static_cast<void>(background_shell_->notify(
                "BlackBox recording paused",
                "No telemetry is recorded while paused."));
        }
        next_dashboard_refresh_at_ = telemetry_clock_.now();
        next_background_refresh_at_ = telemetry_clock_.now();
    }
    if (requested(platform::BackgroundShellCommand::toggle_launch_at_login)) {
        const bool enabled = !background_launch_at_login_enabled_;
        if (background_shell_->set_launch_at_login(enabled)) {
            background_launch_at_login_enabled_ = enabled;
            static_cast<void>(background_shell_->notify(
                enabled ? "BlackBox starts at login" : "BlackBox startup disabled",
                enabled ? "BlackBox will start quietly after you sign in."
                        : "BlackBox will only start when you launch it."));
        } else {
            static_cast<void>(background_shell_->notify(
                "BlackBox startup setting failed",
                "The desktop session did not accept the requested startup change."));
        }
    }
    if (requested(platform::BackgroundShellCommand::toggle_notifications)) {
        const bool enabled = !background_shell_->notifications_enabled();
        background_shell_->set_notifications_enabled(enabled);
        product_settings_.notifications_enabled = enabled;
        product_ui_state_.notifications = enabled;
        if (const auto saved = save_product_settings(product_settings_path_,
                                                     product_settings_); !saved) {
            recorder_settings_status_text_ = "Notification setting applied; save failed: " +
                                             saved.error().message;
        }
        if (enabled) {
            static_cast<void>(background_shell_->notify(
                "BlackBox notifications enabled",
                "Capture lifecycle notifications are visible again."));
        }
    }
}

void Application::show_window() noexcept {
    if (window_ == nullptr) {
        return;
    }
    SDL_ShowWindow(window_);
    SDL_RestoreWindow(window_);
    SDL_RaiseWindow(window_);
    next_dashboard_refresh_at_ = telemetry_clock_.now();
    if (background_shell_ != nullptr) {
        background_shell_->set_window_visible(true);
    }
}

void Application::hide_window() noexcept {
    if (window_ == nullptr || background_shell_ == nullptr ||
        !background_shell_->diagnostics().tray_available) {
        return;
    }
    SDL_HideWindow(window_);
    background_shell_->set_window_visible(false);
}

void Application::request_incident_capture() noexcept {
    if (collector_ == nullptr) {
        return;
    }
    switch (collector_->request_incident_capture()) {
    case core::IncidentCaptureRequestResult::started:
        core::Logger::write(core::LogLevel::info, "Incident capture started");
        if (background_shell_ != nullptr) {
            static_cast<void>(background_shell_->notify(
                "BlackBox capture started",
                "Collecting the configured post-incident window."));
        }
        break;
    case core::IncidentCaptureRequestResult::merged:
        core::Logger::write(core::LogLevel::info,
                            "Incident trigger merged into active capture");
        if (background_shell_ != nullptr) {
            static_cast<void>(background_shell_->notify(
                "BlackBox capture extended",
                "The new trigger was merged into the active incident."));
        }
        break;
    case core::IncidentCaptureRequestResult::queue_full:
        core::Logger::write(core::LogLevel::warning,
                            "Incident capture rejected: writer queue full");
        if (background_shell_ != nullptr) {
            static_cast<void>(background_shell_->notify(
                "BlackBox capture failed",
                "The bounded incident queue is full; recording continues."));
        }
        break;
    case core::IncidentCaptureRequestResult::stopped:
        core::Logger::write(core::LogLevel::warning,
                            "Incident capture rejected: recorder stopped");
        if (background_shell_ != nullptr) {
            static_cast<void>(background_shell_->notify(
                "BlackBox capture unavailable",
                "Resume recording before requesting an incident."));
        }
        break;
    }
}

} // namespace blackbox::app
