#include "app/application.hpp"
#include "app/dashboard_projection.hpp"
#include "app/visible_frame_scheduler.hpp"

#include "core/logger.hpp"
#include "core/version.hpp"
#include "ui/dashboard.hpp"
#include "ui/product_ui_model.hpp"

#if defined(_WIN32)
#include "platform/windows/windows_accessibility.hpp"
#include "platform/windows/windows_background_shell.hpp"
#include "platform/windows/windows_crash_diagnostics.hpp"
#include "platform/windows/windows_global_hotkey_manager.hpp"
#include "telemetry/windows/windows_system_event_provider.hpp"
#include "telemetry/windows/windows_telemetry_provider.hpp"
#elif defined(__linux__)
#include "platform/linux/linux_accessibility.hpp"
#include "platform/linux/linux_background_shell.hpp"
#include "platform/linux/linux_global_hotkey_manager.hpp"
#include "platform/posix/posix_crash_diagnostics.hpp"
#include "telemetry/linux/linux_system_event_provider.hpp"
#include "telemetry/linux/linux_telemetry_provider.hpp"
#elif defined(__APPLE__)
#include "platform/macos/macos_accessibility.hpp"
#include "platform/macos/macos_background_shell.hpp"
#include "platform/macos/macos_global_hotkey_manager.hpp"
#include "platform/posix/posix_crash_diagnostics.hpp"
#include "telemetry/macos/macos_system_event_provider.hpp"
#include "telemetry/macos/macos_telemetry_provider.hpp"
#else
#include "telemetry/mock/mock_telemetry_provider.hpp"
#endif

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <utility>

namespace blackbox::app {
namespace {

using namespace std::chrono_literals;

void load_product_font(ImGuiIO& io, const float display_scale) {
#if defined(_WIN32)
    constexpr std::array candidates{"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"};
#elif defined(__APPLE__)
    constexpr std::array candidates{"/System/Library/Fonts/SFNS.ttf",
                                    "/System/Library/Fonts/Helvetica.ttc"};
#else
    constexpr std::array candidates{
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"};
#endif
    for (const auto* candidate : candidates) {
        std::error_code error{};
        if (!std::filesystem::is_regular_file(candidate, error) || error) continue;
        if (io.Fonts->AddFontFromFileTTF(
                candidate, 17.0F * ui::normalize_display_scale(display_scale)) != nullptr) {
            return;
        }
    }
    ImFontConfig fallback{};
    fallback.SizePixels = 13.0F * ui::normalize_display_scale(display_scale);
    static_cast<void>(io.Fonts->AddFontDefault(&fallback));
}

[[nodiscard]] constexpr bool
any_event_source_enabled(const telemetry::EventProviderConfiguration& configuration) noexcept {
    return configuration.power_events || configuration.device_events ||
           configuration.audio_device_events || configuration.service_events ||
           configuration.security_events || configuration.update_events ||
           configuration.application_events || configuration.network_events ||
           configuration.graphics_events || configuration.storage_events;
}

[[nodiscard]] constexpr ui::MetricDisplayStatus
display_status(const telemetry::MetricStatus status) noexcept {
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

[[nodiscard]] constexpr std::uint32_t
command_bit(const platform::BackgroundShellCommand command) noexcept {
    return 1U << static_cast<std::uint32_t>(command);
}

[[nodiscard]] constexpr bool is_direct_ui_interaction(const std::uint32_t event_type) noexcept {
    switch (event_type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
        return true;
    default:
        return false;
    }
}

} // namespace

Application::Application(const bool start_hidden,
                         ApplicationDiagnosticOptions diagnostic_options) noexcept
    : start_hidden_{start_hidden}, diagnostic_options_{std::move(diagnostic_options)} {}

Application::~Application() { shutdown(); }

ApplicationInitializationResult Application::initialize() {
    product_settings_path_ = default_product_settings_path();
#if defined(_WIN32)
    crash_diagnostics_ = std::make_unique<platform::windows::WindowsCrashDiagnostics>(
        product_settings_path_.parent_path() / "crashes");
    static_cast<void>(crash_diagnostics_->install());
#elif defined(__linux__) || defined(__APPLE__)
    crash_diagnostics_ = std::make_unique<platform::posix::PosixCrashDiagnostics>(
        product_settings_path_.parent_path() / "crashes");
    static_cast<void>(crash_diagnostics_->install());
#endif
    const auto loaded_product_settings = load_product_settings(product_settings_path_);
    if (loaded_product_settings) {
        product_settings_ = *loaded_product_settings;
    } else {
        const auto defaults = validate_product_settings(default_product_settings());
        if (!defaults) {
            core::Logger::write(core::LogLevel::error, "Default product settings are invalid");
            return ApplicationInitializationResult::failed;
        }
        product_settings_ = *defaults;
        recorder_settings_status_text_ =
            "Invalid product settings; using defaults: " + loaded_product_settings.error().message;
    }
    synchronize_product_ui();
#if defined(__linux__) || defined(__APPLE__)
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        core::Logger::write(core::LogLevel::error, SDL_GetError());
        return ApplicationInitializationResult::failed;
    }
    sdl_initialized_ = true;
#if defined(__linux__)
    background_shell_ = std::make_unique<platform::linux::LinuxBackgroundShell>();
#else
    background_shell_ = std::make_unique<platform::macos::MacosBackgroundShell>();
#endif
    const auto shell_result =
        background_shell_->start([this](const platform::BackgroundShellCommand command) {
            pending_background_commands_.fetch_or(command_bit(command), std::memory_order_release);
        });
    if (shell_result == platform::BackgroundShellStartResult::already_running) {
        background_shell_.reset();
        return ApplicationInitializationResult::already_running;
    }
    if (shell_result == platform::BackgroundShellStartResult::started) {
        background_shell_started_ = true;
        background_launch_at_login_enabled_ = background_shell_->launch_at_login_enabled();
        const auto tray_available = background_shell_->diagnostics().tray_available;
        background_status_text_ =
            tray_available ? "Desktop tray active" : "Desktop tray unavailable; close exits";
        background_shell_->set_notifications_enabled(product_settings_.notifications_enabled);
    } else {
        background_shell_.reset();
        background_status_text_ = "Desktop shell unavailable; close exits";
    }
#if defined(__linux__)
    linux_accessibility_monitor_ = std::make_unique<platform::linux::LinuxAccessibilityMonitor>();
    if (linux_accessibility_monitor_->start()) {
        linux_accessibility_monitor_->request_refresh();
    } else {
        linux_accessibility_monitor_.reset();
    }
#endif
#endif
#if defined(_WIN32)
    background_shell_ = std::make_unique<platform::windows::WindowsBackgroundShell>();
    const auto shell_result =
        background_shell_->start([this](const platform::BackgroundShellCommand command) {
            pending_background_commands_.fetch_or(command_bit(command), std::memory_order_release);
        });
    if (shell_result == platform::BackgroundShellStartResult::already_running) {
        background_shell_.reset();
        return ApplicationInitializationResult::already_running;
    }
    if (shell_result == platform::BackgroundShellStartResult::started) {
        background_shell_started_ = true;
        background_launch_at_login_enabled_ = background_shell_->launch_at_login_enabled();
        background_status_text_ = "Tray active";
        background_shell_->set_notifications_enabled(product_settings_.notifications_enabled);
    } else {
        background_shell_.reset();
        background_status_text_ = "Tray unavailable; close exits";
    }

    telemetry_provider_ =
        std::make_unique<telemetry::windows::WindowsTelemetryProvider>(telemetry_clock_);
    system_event_provider_ = std::make_unique<telemetry::windows::WindowsSystemEventProvider>();
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
    system_event_provider_ = std::make_unique<telemetry::linux::LinuxSystemEventProvider>();
    provider_name_ = "LinuxTelemetryProvider";
#elif defined(__APPLE__)
    telemetry_provider_ =
        std::make_unique<telemetry::macos::MacosTelemetryProvider>(telemetry_clock_);
    system_event_provider_ = std::make_unique<telemetry::macos::MacosSystemEventProvider>();
    provider_name_ = "MacosTelemetryProvider";
#else
    telemetry_provider_ =
        std::make_unique<telemetry::mock::MockTelemetryProvider>(telemetry_clock_);
    provider_name_ = "MockTelemetryProvider";
#endif

#if defined(__linux__) || defined(__APPLE__)
    auto event_configuration = telemetry::EventCollectorConfiguration{};
    event_configuration.provider = event_provider_configuration(product_settings_);
    event_configuration.automatic_system_event_capture = false;
    system_event_collector_ = std::make_unique<telemetry::SystemEventCollector>(
        *system_event_provider_, telemetry_clock_, event_configuration);
#endif

    recorder_settings_path_ = default_recorder_settings_path();
    const auto loaded_settings = load_recorder_settings(recorder_settings_path_);
    telemetry::ValidatedRecorderConfiguration configuration{};
    if (!loaded_settings) {
        recorder_settings_status_text_ =
            "Invalid settings; using conservative defaults: " + loaded_settings.error().message;
        const auto defaults = telemetry::validate_recorder_configuration({});
        if (!defaults) {
            core::Logger::write(core::LogLevel::error, "Default recorder configuration is invalid");
            return ApplicationInitializationResult::failed;
        }
        configuration = *defaults;
    } else {
        configuration = *loaded_settings;
        recorder_settings_status_text_ = "Loaded from " + recorder_settings_path_.string();
    }
    product_ui_state_.collect_process_paths = configuration.values.collect_process_paths;
    product_ui_state_.incident_pre_window_seconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(configuration.values.incident_pre_window)
            .count());
    product_ui_state_.incident_post_window_seconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(configuration.values.incident_post_window)
            .count());
#if BLACKBOX_AUTOMATIC_DETECTION_ENABLED
    automatic_detector_ = std::make_unique<telemetry::AutomaticIncidentDetector>(
        detector_configuration(product_settings_));
    collector_ = std::make_unique<telemetry::TelemetryCollector>(
        *telemetry_provider_, telemetry_clock_, configuration, automatic_detector_.get(),
        system_event_collector_.get(), system_event_collector_.get(),
        system_event_collector_.get());
    collector_->set_automatic_detection_enabled(product_settings_.automatic_detection_enabled);
#else
    collector_ = std::make_unique<telemetry::TelemetryCollector>(
        *telemetry_provider_, telemetry_clock_, configuration, nullptr,
        system_event_collector_.get(), system_event_collector_.get(),
        system_event_collector_.get());
#endif
    collector_->set_foreground_application_enabled(product_settings_.record_foreground_application);
    collector_->set_process_lifecycle_enabled(product_settings_.record_process_lifecycle);
    if (system_event_collector_ != nullptr) {
        system_event_collector_->set_incident_capture_sink(collector_.get());
    }

#if BLACKBOX_STORAGE_ENABLED
    auto archive_configuration = storage::ArchiveConfiguration{};
    archive_configuration.path = product_settings_.archive_path;
    archive_configuration.maximum_bytes = product_settings_.archive_maximum_bytes;
    incident_archive_ =
        std::make_unique<storage::SqliteIncidentArchive>(std::move(archive_configuration));
    if (const auto opened = incident_archive_->open(); opened.has_value()) {
        storage_status_text_ = "Ready";
        if (const auto count = incident_archive_->incident_count(); count.has_value()) {
            stored_incidents_at_start_ = *count;
        }
    } else {
        storage_status_text_ = "Unavailable: " + opened.error().message;
    }
    incident_writer_ = std::make_unique<storage::IncidentWriter>(collector_->incident_work_source(),
                                                                 *incident_archive_);
    archive_maintenance_service_ =
        std::make_unique<ArchiveMaintenanceService>(*incident_archive_, *incident_writer_);
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
    display_scale_ = ui::normalize_display_scale(SDL_GetWindowDisplayScale(window_));
    load_product_font(io, display_scale_);
#if defined(_WIN32)
    const auto accessibility = platform::windows::accessibility_preferences();
#elif defined(__APPLE__)
    const auto accessibility = platform::macos::accessibility_preferences();
#elif defined(__linux__)
    const auto accessibility = linux_accessibility_monitor_ != nullptr
                                   ? linux_accessibility_monitor_->snapshot().preferences
                                   : platform::AccessibilityPreferences{};
#endif
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    high_contrast_enabled_ = accessibility.high_contrast;
    animations_enabled_ = accessibility.animations_enabled;
#endif
    ui::apply_accessibility_style(high_contrast_enabled_, display_scale_);

    if (!ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_)) {
        core::Logger::write(core::LogLevel::error, "Failed to initialize the ImGui SDL3 backend");
        return ApplicationInitializationResult::failed;
    }
    imgui_sdl_backend_initialized_ = true;
    if (!ImGui_ImplSDLRenderer3_Init(renderer_)) {
        core::Logger::write(core::LogLevel::error,
                            "Failed to initialize the ImGui SDL renderer backend");
        return ApplicationInitializationResult::failed;
    }
    imgui_renderer_backend_initialized_ = true;
    support_bundle_service_.start();

    dashboard_state_.platform_name = BLACKBOX_PLATFORM_NAME;
#if defined(_WIN32)
    dashboard_state_.system_modifier_label = "Windows";
#elif defined(__APPLE__)
    dashboard_state_.system_modifier_label = "Command";
#elif defined(__linux__)
    dashboard_state_.system_modifier_label = "Super";
#endif
    dashboard_state_.provider_name = provider_name_;
    const auto platform_capabilities = telemetry_provider_->capabilities();
    dashboard_state_.foreground_identity_supported = platform_capabilities.foreground_application;
    const auto whole_device_gpu_available = platform_capabilities.gpu_usage;
#if defined(__linux__)
    dashboard_state_.gpu_usage_support =
        whole_device_gpu_available ? "Whole-device GPU evidence is available through a "
                                     "capability-gated "
                                     "native backend"
                                   : "GPU inventory remains available; whole-device usage needs a "
                                     "readable AMD sysfs or NVIDIA NVML backend";
    dashboard_state_.foreground_identity_support =
        platform_capabilities.foreground_application
            ? "Available through privacy-bounded X11 identity"
            : "Unavailable on Wayland: no standardized permission-bounded "
              "active-window identity API";
#elif defined(__APPLE__)
    dashboard_state_.gpu_usage_support =
        whole_device_gpu_available
            ? "Whole-system GPU utilization is available through the native provider"
            : "Whole-system GPU utilization is unavailable through the public "
              "contract; inventory and BlackBox renderer health remain separate "
              "evidence";
    dashboard_state_.foreground_identity_support =
        "Available through the public frontmost-application process identity";
#elif defined(_WIN32)
    dashboard_state_.gpu_usage_support =
        whole_device_gpu_available
            ? "Whole-device GPU engine and memory evidence is available through native counters"
            : "GPU inventory remains available; whole-device usage counters are unavailable";
    dashboard_state_.foreground_identity_support =
        "Available through the native foreground process identity";
#endif
    const auto* renderer_name = SDL_GetRendererName(renderer_);
    dashboard_state_.renderer_backend = renderer_name != nullptr ? renderer_name : "Unknown";
    dashboard_state_.renderer_active = imgui_renderer_backend_initialized_;
    const auto gpu_inventory = telemetry_provider_->gpu_inventory();
    dashboard_state_.gpu_inventory_status = display_status(gpu_inventory.device_count.status);
    if (gpu_inventory.device_count.has_value()) {
        dashboard_state_.gpu_device_count = gpu_inventory.device_count.value;
    }
    if (gpu_inventory.integrated_device_count.has_value()) {
        dashboard_state_.gpu_integrated_device_count = gpu_inventory.integrated_device_count.value;
    }
    if (gpu_inventory.discrete_device_count.has_value()) {
        dashboard_state_.gpu_discrete_device_count = gpu_inventory.discrete_device_count.value;
    }
    if (gpu_inventory.unknown_device_count.has_value()) {
        dashboard_state_.gpu_unknown_device_count = gpu_inventory.unknown_device_count.value;
    }
    if (gpu_inventory.render_device_available.has_value()) {
        dashboard_state_.gpu_render_device_available = gpu_inventory.render_device_available.value;
    }
    core::Logger::write(core::LogLevel::info, std::string{"Renderer initialized: backend="} +
                                                  dashboard_state_.renderer_backend);
    dashboard_state_.background_status = background_status_text_;
    dashboard_state_.accessibility_high_contrast = high_contrast_enabled_;
    dashboard_state_.accessibility_animations_enabled = animations_enabled_;
    refresh_display_metrics();
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
    if (system_event_collector_ != nullptr &&
        any_event_source_enabled(event_provider_configuration(product_settings_))) {
        system_event_collector_->start();
    }
#if defined(_WIN32)
    hotkey_manager_ = std::make_unique<platform::windows::WindowsGlobalHotkeyManager>();
    static_cast<void>(register_configured_hotkey(product_settings_.incident_hotkey));
#elif defined(__linux__)
    hotkey_manager_ = std::make_unique<platform::linux::LinuxGlobalHotkeyManager>();
    static_cast<void>(register_configured_hotkey(product_settings_.incident_hotkey));
#elif defined(__APPLE__)
    hotkey_manager_ = std::make_unique<platform::macos::MacosGlobalHotkeyManager>();
    static_cast<void>(register_configured_hotkey(product_settings_.incident_hotkey));
#endif
    refresh_hotkey_status();

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
    VisibleFrameScheduler visible_frames;
    const auto diagnostic_started_at = telemetry_clock_.now();
    diagnostic_started_ = diagnostic_options_.runtime > 0s;
    if (diagnostic_started_) {
        diagnostic_monotonic_anchor_ = diagnostic_started_at;
        diagnostic_utc_anchor_ = std::chrono::system_clock::now();
    }
    const auto diagnostic_exit_at = diagnostic_started_
                                        ? diagnostic_started_at + diagnostic_options_.runtime
                                        : core::MonotonicTimePoint::max();
    auto next_diagnostic_capture =
        diagnostic_started_ && diagnostic_options_.capture_interval > 0s
            ? diagnostic_started_at + diagnostic_options_.capture_interval
            : core::MonotonicTimePoint::max();
    const auto process_event = [this, &running, &visible_frames](SDL_Event& event) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (is_direct_ui_interaction(event.type)) {
            visible_frames.note_interaction(telemetry_clock_.now());
        }
        if (event.type == SDL_EVENT_QUIT) {
            running = false;
            return;
        }
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            event.window.windowID == SDL_GetWindowID(window_)) {
            if (background_shell_started_ && background_shell_->diagnostics().tray_available) {
                hide_window();
            } else {
                running = false;
            }
        }
        const bool display_window_event = event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED ||
                                          event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED ||
                                          event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
        if (display_window_event && event.window.windowID == SDL_GetWindowID(window_)) {
            refresh_display_metrics(event.type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED);
        } else if (event.type == SDL_EVENT_DISPLAY_ADDED ||
                   event.type == SDL_EVENT_DISPLAY_REMOVED ||
                   event.type == SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED ||
                   event.type == SDL_EVENT_DISPLAY_USABLE_BOUNDS_CHANGED) {
            refresh_display_metrics(event.type == SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED);
        }
    };

    while (running) {
        const auto now = telemetry_clock_.now();
        if (now >= diagnostic_exit_at) {
            diagnostic_completed_ = diagnostic_started_;
            break;
        }
        if (now >= next_diagnostic_capture) {
            const auto post_window =
                collector_ != nullptr ? collector_->diagnostics().configuration.incident_post_window
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
            // Hidden operation avoids dashboard snapshots, process-table
            // copies, and rendering. The lightweight shell refresh remains
            // bounded at 4 Hz.
            if (SDL_WaitEventTimeout(&event, 250)) {
                process_event(event);
            }
            visible_frames.reset();
            continue;
        }

        const auto visible_now = telemetry_clock_.now();
        if (!visible_frames.frame_due(visible_now)) {
            const auto timeout = visible_frames.wait_timeout(visible_now);
            if (SDL_WaitEventTimeout(&event, static_cast<int>(timeout.count()))) {
                process_event(event);
            }
            continue;
        }

        refresh_dashboard_if_due();

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const auto command =
            ui::render_dashboard(dashboard_state_, incident_viewer_state_, product_ui_state_);
        switch (command.action) {
        case ui::DashboardAction::none:
            break;
        case ui::DashboardAction::capture_incident:
            request_incident_capture();
            break;
        case ui::DashboardAction::apply_recorder_settings: {
            auto values = collector_->diagnostics().configuration;
            values.sample_interval =
                std::chrono::milliseconds{command.sample_interval_milliseconds};
            values.history_duration = std::chrono::seconds{command.history_duration_seconds};
            auto validated = telemetry::validate_recorder_configuration(values);
            if (!validated) {
                recorder_settings_status_text_ = "Rejected: settings exceed recorder bounds";
                break;
            }
            collector_->reconfigure(*validated);
            if (const auto saved = save_recorder_settings(recorder_settings_path_, *validated);
                !saved) {
                recorder_settings_status_text_ =
                    "Applied for this session; save failed: " + saved.error().message;
            } else {
                recorder_settings_status_text_ =
                    "Applied and saved to " + recorder_settings_path_.string();
            }
            break;
        }
        case ui::DashboardAction::apply_product_settings:
            apply_product_settings(command);
            break;
        case ui::DashboardAction::complete_onboarding:
            product_settings_.onboarding_completed = true;
            product_ui_state_.onboarding_open = false;
            if (const auto saved = save_product_settings(product_settings_path_, product_settings_);
                !saved) {
                recorder_settings_status_text_ =
                    "Onboarding completed; save failed: " + saved.error().message;
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
                static_cast<storage::ContributorFeedbackResource>(command.contributor_resource),
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
            archive_maintenance_service_->restore(command.restore_path, command.safety_backup_path);
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
    product_ui_state_.hotkey_system_modifier = product_settings_.incident_hotkey.system_modifier;
    product_ui_state_.automatic_detection = product_settings_.automatic_detection_enabled;
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
    product_ui_state_.record_process_lifecycle = product_settings_.record_process_lifecycle;
    product_ui_state_.record_power_and_device_events =
        product_settings_.record_power_and_device_events;
    product_ui_state_.record_audio_device_events = product_settings_.record_audio_device_events;
    product_ui_state_.record_system_event_evidence = product_settings_.record_system_event_evidence;
    product_ui_state_.archive_maximum_mib = product_settings_.archive_maximum_bytes >> 20U;
    copy(product_ui_state_.archive_path, product_settings_.archive_path.string());
    const auto parent = product_settings_.archive_path.parent_path();
    copy(product_ui_state_.backup_path, (parent / "incidents-backup.sqlite3").string());
    copy(product_ui_state_.safety_backup_path, (parent / "incidents-pre-restore.sqlite3").string());
    copy(product_ui_state_.export_path, (parent / "blackbox-evidence-dataset").string());
    copy(product_ui_state_.failed_export_path, (parent / "recoverable-incident.sqlite3").string());
    if (product_ui_state_.support_bundle_path.front() == '\0') {
        copy(product_ui_state_.support_bundle_path, (parent / "blackbox-support-bundle").string());
    }
}

void Application::request_support_bundle(const ui::DashboardCommand& command) noexcept {
    try {
        SupportBundleRequest request{};
        request.destination = command.support_bundle_path;
        auto& value = request.diagnostics;
        value.application_version = std::string{core::version};
        value.platform = BLACKBOX_PLATFORM_NAME;
        value.collector_running = dashboard_state_.recorder_status == "Recording";
        value.automatic_detection_enabled = dashboard_state_.automatic_detection_enabled;
        value.process_path_collection_enabled = product_ui_state_.collect_process_paths;
        value.foreground_identity_enabled = product_settings_.record_foreground_application;
        value.process_lifecycle_enabled = product_settings_.record_process_lifecycle;
        value.power_and_device_events_enabled = product_settings_.record_power_and_device_events;
        value.audio_device_events_enabled = product_settings_.record_audio_device_events;
        value.system_event_evidence_enabled = product_settings_.record_system_event_evidence;
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
        value.incident_snapshot_failures = dashboard_state_.incident_snapshot_failures;
        value.incident_queue_rejections = dashboard_state_.incident_queue_rejections;
        value.automatic_detector_triggers = dashboard_state_.automatic_detector_triggers;
        value.system_events_recorded = dashboard_state_.system_events_recorded;
        value.system_events_dropped = dashboard_state_.system_events_dropped;
        value.process_lifecycle_observations = dashboard_state_.process_lifecycle_observations;
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
        value.recoverable_incident_available = dashboard_state_.archive_recoverable_incident;
        value.previous_crash_evidence = dashboard_state_.previous_crash_evidence;
        if (command.include_latest_crash_evidence && crash_diagnostics_ != nullptr) {
            const auto crash = crash_diagnostics_->snapshot();
            if (!crash.latest_evidence.empty()) {
                request.consented_crash_evidence = crash.latest_evidence;
                request.crash_evidence_disclosure_confirmed =
                    command.crash_evidence_disclosure_confirmed;
            }
        }
        support_bundle_service_.create(std::move(request));
    } catch (...) {
        core::Logger::write(core::LogLevel::error, "Support bundle request could not be prepared");
    }
}

bool Application::register_configured_hotkey(
    const platform::HotkeyCombination combination) noexcept {
    if (hotkey_manager_ == nullptr) return false;
    const auto result =
        hotkey_manager_->register_hotkey(combination, [this] { request_incident_capture(); });
    std::string name;
    if (combination.control) name += "Ctrl+";
    if (combination.shift) name += "Shift+";
    if (combination.alt) name += "Alt+";
    if (combination.system_modifier) {
#if defined(__APPLE__)
        name += "Cmd+";
#elif defined(__linux__)
        name += "Super+";
#else
        name += "Win+";
#endif
    }
    name += "F" + std::to_string(static_cast<unsigned>(combination.key));
    hotkey_display_name_ = name;
    switch (result) {
    case platform::HotkeyRegistrationResult::registered:
        hotkey_status_ = name + " registered";
        return true;
    case platform::HotkeyRegistrationResult::conflict:
        hotkey_status_ = name + " is already in use";
        break;
    case platform::HotkeyRegistrationResult::permission_required:
        hotkey_status_ = "Input Monitoring permission required; grant access "
                         "and apply again";
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

void Application::refresh_hotkey_status() noexcept {
#if defined(__linux__)
    if (hotkey_manager_ != nullptr) {
        const auto* linux_manager =
            static_cast<const platform::linux::LinuxGlobalHotkeyManager*>(hotkey_manager_.get());
        const auto portal = linux_manager->diagnostics();
        if (portal.state == platform::linux::PortalShortcutState::active) {
            hotkey_status_ = hotkey_display_name_ + " registered through portal";
        } else if (portal.state == platform::linux::PortalShortcutState::reconnecting) {
            hotkey_status_ = hotkey_display_name_ + " reconnecting to desktop portal";
        } else if (portal.state == platform::linux::PortalShortcutState::unavailable) {
            hotkey_status_ = "Desktop shortcut removed; reapply it in Settings";
        }
    }
#endif
    dashboard_state_.hotkey_status = hotkey_status_;
}

void Application::apply_product_settings(const ui::DashboardCommand& command) noexcept {
    auto proposed = product_settings_;
    proposed.incident_hotkey = {static_cast<platform::HotkeyKey>(command.hotkey_key),
                                command.hotkey_control, command.hotkey_shift, command.hotkey_alt,
                                command.hotkey_system_modifier};
    proposed.automatic_detection_enabled = command.automatic_detection;
    proposed.detector_sensitivity = static_cast<DetectorSensitivity>(command.detector_sensitivity);
    proposed.detect_cpu = command.detect_cpu;
    proposed.detect_memory = command.detect_memory;
    proposed.detect_disk = command.detect_disk;
    proposed.detect_network = command.detect_network;
    proposed.detector_cooldown = std::chrono::seconds{command.detector_cooldown_seconds};
    proposed.notifications_enabled = command.notifications;
    proposed.record_foreground_application = command.record_foreground_application;
    proposed.record_process_lifecycle = command.record_process_lifecycle;
    proposed.record_power_and_device_events = command.record_power_and_device_events;
    proposed.record_audio_device_events = command.record_audio_device_events;
    proposed.record_system_event_evidence = command.record_system_event_evidence;
    proposed.archive_path = command.archive_path;
    if (command.archive_maximum_mib > (maximum_archive_bytes >> 20U)) {
        recorder_settings_status_text_ = "Rejected: archive capacity exceeds 64 GiB";
        return;
    }
    proposed.archive_maximum_bytes = command.archive_maximum_mib << 20U;
    const auto validated_product = validate_product_settings(proposed);
    if (!validated_product) {
        recorder_settings_status_text_ = "Rejected: " + validated_product.error().message;
        return;
    }

    auto recorder = collector_->diagnostics().configuration;
    recorder.incident_pre_window = std::chrono::seconds{command.incident_pre_window_seconds};
    recorder.incident_post_window = std::chrono::seconds{command.incident_post_window_seconds};
    recorder.collect_process_paths = command.collect_process_paths;
    const auto validated_recorder = telemetry::validate_recorder_configuration(recorder);
    if (!validated_recorder) {
        recorder_settings_status_text_ = "Rejected: capture window or privacy "
                                         "settings exceed recorder bounds";
        return;
    }

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    if (proposed.incident_hotkey != product_settings_.incident_hotkey) {
        hotkey_manager_->unregister_hotkey();
        if (!register_configured_hotkey(proposed.incident_hotkey)) {
            const auto failure = hotkey_status_;
            static_cast<void>(register_configured_hotkey(product_settings_.incident_hotkey));
            recorder_settings_status_text_ = "Rejected: " + failure + "; previous hotkey restored";
            return;
        }
    }
#endif

    const bool restart = collector_->running();
    if (system_event_collector_ != nullptr) system_event_collector_->stop();
    collector_->stop();
#if BLACKBOX_AUTOMATIC_DETECTION_ENABLED
    if (const auto reconfigured =
            automatic_detector_->reconfigure(detector_configuration(*validated_product));
        !reconfigured) {
        if (restart) collector_->start();
        recorder_settings_status_text_ = "Rejected: detector configuration is invalid";
        return;
    }
#endif
    collector_->reconfigure(*validated_recorder);
    collector_->set_foreground_application_enabled(
        validated_product->record_foreground_application);
    collector_->set_process_lifecycle_enabled(validated_product->record_process_lifecycle);
    if (system_event_collector_ != nullptr) {
        auto event_configuration = telemetry::EventCollectorConfiguration{};
        event_configuration.provider = event_provider_configuration(*validated_product);
#if defined(_WIN32)
        event_configuration.automatic_system_event_capture =
            validated_product->automatic_detection_enabled;
#else
        // Linux/macOS currently expose context-only device lifecycle records.
        // They are never eligible to trigger automatic incident capture.
        event_configuration.automatic_system_event_capture = false;
#endif
        system_event_collector_->reconfigure(event_configuration);
    }
    collector_->set_automatic_detection_enabled(validated_product->automatic_detection_enabled);
    if (restart) {
        collector_->start();
        if (system_event_collector_ != nullptr &&
            any_event_source_enabled(event_provider_configuration(*validated_product))) {
            system_event_collector_->start();
        }
    }
    if (background_shell_ != nullptr) {
        background_shell_->set_notifications_enabled(validated_product->notifications_enabled);
    }
    product_settings_ = *validated_product;
    product_ui_state_.collect_process_paths = recorder.collect_process_paths;
    product_ui_state_.incident_pre_window_seconds = command.incident_pre_window_seconds;
    product_ui_state_.incident_post_window_seconds = command.incident_post_window_seconds;

    const auto saved_product = save_product_settings(product_settings_path_, product_settings_);
    const auto saved_recorder =
        save_recorder_settings(recorder_settings_path_, *validated_recorder);
    if (!saved_product || !saved_recorder) {
        recorder_settings_status_text_ = "Applied for this session; one or more "
                                         "settings files could not be saved";
    } else {
        recorder_settings_status_text_ = "Applied and saved. Archive path/capacity "
                                         "changes take effect next launch.";
    }
    refresh_hotkey_status();
    next_dashboard_refresh_at_ = telemetry_clock_.now();
}

void Application::refresh_display_metrics(const bool force_style_refresh) {
    if (window_ == nullptr) return;
    const auto raw_scale = SDL_GetWindowDisplayScale(window_);
    const auto requested_scale =
        raw_scale > 0.0F ? ui::normalize_display_scale(raw_scale) : display_scale_;
    const bool scale_changed = ui::display_scale_changed(display_scale_, requested_scale);
    if ((scale_changed || force_style_refresh) && ImGui::GetCurrentContext() != nullptr) {
        display_scale_ = requested_scale;
        if (scale_changed) {
            auto& io = ImGui::GetIO();
            io.Fonts->Clear();
            load_product_font(io, display_scale_);
        }
        ui::apply_accessibility_style(high_contrast_enabled_, display_scale_);
    } else if (scale_changed) {
        display_scale_ = requested_scale;
    }

    dashboard_state_.display_scale = display_scale_;
    int pixel_width{};
    int pixel_height{};
    if (SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height)) {
        dashboard_state_.window_pixel_width = static_cast<std::uint32_t>(std::max(0, pixel_width));
        dashboard_state_.window_pixel_height =
            static_cast<std::uint32_t>(std::max(0, pixel_height));
    }
    int display_count{};
    SDL_DisplayID* displays = SDL_GetDisplays(&display_count);
    dashboard_state_.display_count = static_cast<std::uint32_t>(std::max(0, display_count));
    SDL_free(displays);
}

void Application::refresh_accessibility_if_due() {
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    const auto now = telemetry_clock_.now();
    if (now < next_accessibility_refresh_at_) return;
    do {
        next_accessibility_refresh_at_ += 1s;
    } while (next_accessibility_refresh_at_ <= now);

#if defined(_WIN32)
    const auto accessibility = platform::windows::accessibility_preferences();
#elif defined(__APPLE__)
    const auto accessibility = platform::macos::accessibility_preferences();
#else
    if (linux_accessibility_monitor_ == nullptr) return;
    linux_accessibility_monitor_->request_refresh();
    const auto accessibility = linux_accessibility_monitor_->snapshot().preferences;
#endif
    static_cast<void>(ui::update_accessibility_style(high_contrast_enabled_,
                                                     accessibility.high_contrast, display_scale_));
    animations_enabled_ = accessibility.animations_enabled;
    dashboard_state_.accessibility_high_contrast = high_contrast_enabled_;
    dashboard_state_.accessibility_animations_enabled = animations_enabled_;
#endif
}

void Application::refresh_background_shell_if_due() {
    const auto now = telemetry_clock_.now();
    if (!background_shell_started_ || background_shell_ == nullptr || collector_ == nullptr ||
        now < next_background_refresh_at_) {
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

    if (collector_diagnostics.automatic_captures_started > background_last_automatic_triggers_) {
        background_last_automatic_triggers_ = collector_diagnostics.automatic_captures_started;
        static_cast<void>(background_shell_->notify(
            "BlackBox capture started",
            "An unusual resource pattern triggered an incident capture."));
    }
    if (collector_diagnostics.incident_capture.snapshot_failures >
        background_last_snapshot_failures_) {
        background_last_snapshot_failures_ =
            collector_diagnostics.incident_capture.snapshot_failures;
        static_cast<void>(background_shell_->notify("BlackBox capture failed",
                                                    "The immutable incident snapshot could not "
                                                    "be constructed; recording continues."));
    }
    if (collector_diagnostics.incident_capture.captures_cancelled >
        background_last_capture_cancellations_) {
        background_last_capture_cancellations_ =
            collector_diagnostics.incident_capture.captures_cancelled;
        static_cast<void>(
            background_shell_->notify("BlackBox capture cancelled",
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
                "BlackBox capture completed", "The incident was saved to the local archive."));
        }
        if (writer.failed > background_last_writer_failures_) {
            background_last_writer_failures_ = writer.failed;
            const auto detail =
                writer.last_error_message.empty()
                    ? std::string{"The local archive rejected the incident."}
                    : std::string{"The incident could not be saved: "} + writer.last_error_message;
            static_cast<void>(background_shell_->notify("BlackBox capture failed", detail));
        }
    }
#endif

    background_shell_->set_status(status);
    const auto shell = background_shell_->diagnostics();
    background_status_text_ = shell.tray_available ? "Tray active" : "Tray unavailable";
    background_status_text_ += shell.window_visible ? " | window visible" : " | window hidden";
#if defined(__linux__)
    background_status_text_ += !shell.notifications_available      ? " | notifications unavailable"
                               : !shell.notifications_enabled      ? " | notifications quiet"
                               : shell.portal_notifications_active ? " | portal notifications on"
                                                                   : " | notifications on";
    if (shell.background_status_available) {
        background_status_text_ += " | portal background status";
    }
    if (shell.desktop_service_reconnects != 0U) {
        background_status_text_ += " | desktop service recovered";
    }
#else
    background_status_text_ +=
        shell.notifications_enabled ? " | notifications on" : " | notifications quiet";
#endif
    background_status_text_ +=
        background_launch_at_login_enabled_ ? " | starts at login" : " | manual startup";
    if (shell.tray_readd_failures != 0U) {
        background_status_text_ += " | tray recovery failed";
    }
    dashboard_state_.background_status = background_status_text_;
}

void Application::process_background_commands(bool& running) {
    const auto commands = pending_background_commands_.exchange(0U, std::memory_order_acquire);
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
    if (requested(platform::BackgroundShellCommand::toggle_recording) && collector_ != nullptr) {
        if (recorder_paused_) {
            collector_->start();
            if (system_event_collector_ != nullptr &&
                any_event_source_enabled(event_provider_configuration(product_settings_))) {
                system_event_collector_->start();
            }
            recorder_paused_ = false;
            static_cast<void>(background_shell_->notify(
                "BlackBox recording resumed", "The bounded rolling recorder is active again."));
        } else {
            if (system_event_collector_ != nullptr) system_event_collector_->stop();
            collector_->stop();
            recorder_paused_ = true;
            static_cast<void>(background_shell_->notify("BlackBox recording paused",
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
            static_cast<void>(background_shell_->notify("BlackBox startup setting failed",
                                                        "The desktop session did not accept "
                                                        "the requested startup change."));
        }
    }
    if (requested(platform::BackgroundShellCommand::toggle_notifications)) {
        const bool enabled = !background_shell_->notifications_enabled();
        background_shell_->set_notifications_enabled(enabled);
        product_settings_.notifications_enabled = enabled;
        product_ui_state_.notifications = enabled;
        if (const auto saved = save_product_settings(product_settings_path_, product_settings_);
            !saved) {
            recorder_settings_status_text_ =
                "Notification setting applied; save failed: " + saved.error().message;
        }
        if (enabled) {
            static_cast<void>(
                background_shell_->notify("BlackBox notifications enabled",
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
                "BlackBox capture started", "Collecting the configured post-incident window."));
        }
        break;
    case core::IncidentCaptureRequestResult::merged:
        core::Logger::write(core::LogLevel::info, "Incident trigger merged into active capture");
        if (background_shell_ != nullptr) {
            static_cast<void>(
                background_shell_->notify("BlackBox capture extended",
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
        core::Logger::write(core::LogLevel::warning, "Incident capture rejected: recorder stopped");
        if (background_shell_ != nullptr) {
            static_cast<void>(background_shell_->notify(
                "BlackBox capture unavailable", "Resume recording before requesting an incident."));
        }
        break;
    }
}

} // namespace blackbox::app
