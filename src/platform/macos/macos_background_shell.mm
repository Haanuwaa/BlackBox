#include "platform/macos/macos_background_shell.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_tray.h>

#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>
#import <UserNotifications/UserNotifications.h>

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <utility>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

namespace blackbox::platform::macos {
namespace {

[[nodiscard]] std::filesystem::path default_state_directory() {
    @autoreleasepool {
        const auto paths = NSSearchPathForDirectoriesInDomains(
            NSApplicationSupportDirectory, NSUserDomainMask, YES);
        if (paths.count == 0U) return {};
        const auto* path = paths.firstObject;
        if (path == nil || path.fileSystemRepresentation == nullptr) return {};
        return std::filesystem::path{path.fileSystemRepresentation} /
               "io.github.Haanuwaa.BlackBox";
    }
}

[[nodiscard]] bool is_plain_directory(const std::filesystem::path& path) noexcept {
    std::error_code issue{};
    const auto status = std::filesystem::symlink_status(path, issue);
    return !issue && std::filesystem::is_directory(status) &&
           !std::filesystem::is_symlink(status);
}

[[nodiscard]] bool uses_application_bundle() noexcept {
    @autoreleasepool {
        const auto* path_extension = NSBundle.mainBundle.bundleURL.pathExtension;
        return path_extension != nil &&
               [path_extension caseInsensitiveCompare:@"app"] == NSOrderedSame;
    }
}

enum class NotificationAuthorization : std::uint8_t {
    unknown,
    requesting,
    authorized,
    denied,
};

[[nodiscard]] NotificationAuthorization authorization_from(
    const UNAuthorizationStatus status) noexcept {
    switch (status) {
    case UNAuthorizationStatusAuthorized:
    case UNAuthorizationStatusProvisional:
        return NotificationAuthorization::authorized;
    case UNAuthorizationStatusDenied:
        return NotificationAuthorization::denied;
    case UNAuthorizationStatusNotDetermined:
        return NotificationAuthorization::unknown;
    }
    return NotificationAuthorization::unknown;
}

struct AsyncNotificationState {
    std::mutex mutex{};
    std::atomic<NotificationAuthorization> authorization{
        NotificationAuthorization::unknown};
    std::uint64_t sent{};
    std::uint64_t dropped{};
    std::size_t pending{};
};

constexpr std::size_t maximum_pending_notifications = 8U;

void refresh_notification_authorization(
    const std::shared_ptr<AsyncNotificationState>& state) noexcept {
    @autoreleasepool {
        const auto* center = UNUserNotificationCenter.currentNotificationCenter;
        if (center == nil) {
            state->authorization.store(NotificationAuthorization::denied,
                                       std::memory_order_relaxed);
            return;
        }
        [center getNotificationSettingsWithCompletionHandler:
            ^(UNNotificationSettings* settings) {
                const auto authorization = authorization_from(settings.authorizationStatus);
                if (authorization != NotificationAuthorization::unknown ||
                    state->authorization.load(std::memory_order_relaxed) !=
                        NotificationAuthorization::requesting) {
                    state->authorization.store(authorization, std::memory_order_relaxed);
                }
            }];
    }
}

void request_notification_authorization(
    const std::shared_ptr<AsyncNotificationState>& state) noexcept {
    @autoreleasepool {
        const auto* center = UNUserNotificationCenter.currentNotificationCenter;
        [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert
                              completionHandler:^(BOOL granted, NSError* error) {
                                  state->authorization.store(
                                      granted && error == nil
                                          ? NotificationAuthorization::authorized
                                          : NotificationAuthorization::denied,
                                      std::memory_order_relaxed);
                              }];
    }
}

} // namespace

struct MacosBackgroundShell::NativeState {
    explicit NativeState(MacosBackgroundShellOptions value)
        : options{std::move(value)}, notification_state{std::make_shared<AsyncNotificationState>()} {
        if (options.state_directory.empty()) {
            options.state_directory = default_state_directory();
        }
    }

    void dispatch(const BackgroundShellCommand command) noexcept {
        try {
            callback(command);
            const std::scoped_lock lock{mutex};
            ++diagnostics.commands_dispatched;
        } catch (...) {
            // Native desktop callbacks cannot unwind into SDL.
        }
    }

    static void SDLCALL tray_callback(void* userdata, SDL_TrayEntry* entry) {
        auto& state = *static_cast<NativeState*>(userdata);
        if (entry == state.show_hide_entry) {
            state.dispatch(state.window_visible ? BackgroundShellCommand::hide_window
                                                : BackgroundShellCommand::show_window);
        } else if (entry == state.capture_entry) {
            state.dispatch(BackgroundShellCommand::capture_incident);
        } else if (entry == state.pause_entry) {
            state.dispatch(BackgroundShellCommand::toggle_recording);
        } else if (entry == state.autostart_entry) {
            state.dispatch(BackgroundShellCommand::toggle_launch_at_login);
        } else if (entry == state.notifications_entry) {
            state.dispatch(BackgroundShellCommand::toggle_notifications);
        } else if (entry == state.exit_entry) {
            state.dispatch(BackgroundShellCommand::exit_application);
        }
    }

    [[nodiscard]] SDL_TrayEntry* add_entry(SDL_TrayMenu* menu, const char* label,
                                           const SDL_TrayEntryFlags flags) {
        auto* entry = SDL_InsertTrayEntryAt(menu, -1, label, flags);
        if (entry != nullptr && label != nullptr) {
            SDL_SetTrayEntryCallback(entry, &NativeState::tray_callback, this);
        }
        return entry;
    }

    [[nodiscard]] bool create_tray() noexcept {
        if (!options.install_tray_icon ||
            (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0U) {
            return false;
        }
        tray = SDL_CreateTray(nullptr, "BlackBox - recording");
        if (tray == nullptr) return false;
        auto* menu = SDL_CreateTrayMenu(tray);
        if (menu == nullptr) {
            SDL_DestroyTray(tray);
            tray = nullptr;
            return false;
        }
        show_hide_entry = add_entry(menu, "Hide BlackBox", SDL_TRAYENTRY_BUTTON);
        capture_entry = add_entry(menu, "Capture incident", SDL_TRAYENTRY_BUTTON);
        pause_entry = add_entry(menu, "Pause recording", SDL_TRAYENTRY_BUTTON);
        static_cast<void>(add_entry(menu, nullptr, SDL_TRAYENTRY_BUTTON));
        autostart_entry = add_entry(menu, "Start at login", SDL_TRAYENTRY_CHECKBOX);
        notifications_entry = add_entry(menu, "Notifications", SDL_TRAYENTRY_CHECKBOX);
        static_cast<void>(add_entry(menu, nullptr, SDL_TRAYENTRY_BUTTON));
        exit_entry = add_entry(menu, "Exit BlackBox", SDL_TRAYENTRY_BUTTON);
        if (show_hide_entry == nullptr || capture_entry == nullptr ||
            pause_entry == nullptr || autostart_entry == nullptr ||
            notifications_entry == nullptr || exit_entry == nullptr) {
            SDL_DestroyTray(tray);
            tray = nullptr;
            return false;
        }
        SDL_SetTrayEntryChecked(autostart_entry, launch_at_login_enabled_unlocked());
        SDL_SetTrayEntryChecked(notifications_entry,
                                notifications_enabled.load(std::memory_order_relaxed));
        SDL_UpdateTrays();
        return true;
    }

    void destroy_tray() noexcept {
        if (tray != nullptr) SDL_DestroyTray(tray);
        tray = nullptr;
        show_hide_entry = nullptr;
        capture_entry = nullptr;
        pause_entry = nullptr;
        autostart_entry = nullptr;
        notifications_entry = nullptr;
        exit_entry = nullptr;
    }

    [[nodiscard]] bool launch_at_login_enabled_unlocked() const noexcept {
        if (!options.use_native_services || !uses_application_bundle()) return false;
        @autoreleasepool {
            return SMAppService.mainAppService.status == SMAppServiceStatusEnabled;
        }
    }

    void count_notification_drop() noexcept {
        const std::scoped_lock lock{notification_state->mutex};
        ++notification_state->dropped;
    }

    MacosBackgroundShellOptions options{};
    BackgroundShellCallback callback{};
    mutable std::mutex mutex{};
    BackgroundShellDiagnostics diagnostics{};
    std::shared_ptr<AsyncNotificationState> notification_state{};
    int lock_descriptor{-1};
    bool window_visible{true};
    std::atomic_bool notifications_enabled{true};
    BackgroundShellStatus status{BackgroundShellStatus::recording};
    SDL_Tray* tray{};
    SDL_TrayEntry* show_hide_entry{};
    SDL_TrayEntry* capture_entry{};
    SDL_TrayEntry* pause_entry{};
    SDL_TrayEntry* autostart_entry{};
    SDL_TrayEntry* notifications_entry{};
    SDL_TrayEntry* exit_entry{};
};

MacosBackgroundShell::MacosBackgroundShell(MacosBackgroundShellOptions options)
    : native_{std::make_unique<NativeState>(std::move(options))} {}

MacosBackgroundShell::~MacosBackgroundShell() { stop(); }

BackgroundShellStartResult MacosBackgroundShell::start(BackgroundShellCallback callback) {
    stop();
    if (!callback || native_->options.state_directory.empty()) {
        return BackgroundShellStartResult::unavailable;
    }
    std::error_code issue{};
    std::filesystem::create_directories(native_->options.state_directory, issue);
    if (issue || !is_plain_directory(native_->options.state_directory)) {
        return BackgroundShellStartResult::unavailable;
    }
    const auto lock_path = native_->options.state_directory / "instance.lock";
    native_->lock_descriptor = ::open(lock_path.c_str(),
                                      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                                      S_IRUSR | S_IWUSR);
    if (native_->lock_descriptor < 0) return BackgroundShellStartResult::unavailable;
    if (::flock(native_->lock_descriptor, LOCK_EX | LOCK_NB) != 0) {
        const auto conflict = errno == EWOULDBLOCK || errno == EAGAIN;
        ::close(native_->lock_descriptor);
        native_->lock_descriptor = -1;
        return conflict ? BackgroundShellStartResult::already_running
                        : BackgroundShellStartResult::unavailable;
    }
    native_->callback = std::move(callback);
    const bool tray_available = native_->create_tray();
    if (native_->options.use_native_services) {
        refresh_notification_authorization(native_->notification_state);
    }
    const bool notifications_available = native_->options.use_native_services;
    {
        const std::scoped_lock lock{native_->mutex};
        native_->diagnostics = {};
        native_->diagnostics.running = true;
        native_->diagnostics.tray_available = tray_available;
        native_->diagnostics.window_visible = native_->window_visible;
        native_->diagnostics.notifications_available = notifications_available;
        native_->diagnostics.notifications_enabled =
            native_->notifications_enabled.load(std::memory_order_relaxed);
    }
    return BackgroundShellStartResult::started;
}

void MacosBackgroundShell::stop() noexcept {
    native_->destroy_tray();
    if (native_->lock_descriptor >= 0) {
        static_cast<void>(::flock(native_->lock_descriptor, LOCK_UN));
        ::close(native_->lock_descriptor);
        native_->lock_descriptor = -1;
    }
    native_->callback = {};
    const std::scoped_lock lock{native_->mutex};
    native_->diagnostics.running = false;
    native_->diagnostics.tray_available = false;
}

void MacosBackgroundShell::set_status(const BackgroundShellStatus status) noexcept {
    native_->status = status;
    if (native_->tray == nullptr) return;
    const bool paused = status == BackgroundShellStatus::paused;
    SDL_SetTrayEntryLabel(native_->pause_entry,
                          paused ? "Resume recording" : "Pause recording");
    const char* tooltip = "BlackBox - recording";
    if (status == BackgroundShellStatus::capturing) tooltip = "BlackBox - capturing incident";
    else if (paused) tooltip = "BlackBox - paused";
    else if (status == BackgroundShellStatus::retrying_storage) tooltip = "BlackBox - retrying archive";
    else if (status == BackgroundShellStatus::error) tooltip = "BlackBox - attention required";
    SDL_SetTrayTooltip(native_->tray, tooltip);
    SDL_UpdateTrays();
}

void MacosBackgroundShell::set_window_visible(const bool visible) noexcept {
    native_->window_visible = visible;
    if (native_->show_hide_entry != nullptr) {
        SDL_SetTrayEntryLabel(native_->show_hide_entry,
                              visible ? "Hide BlackBox" : "Show BlackBox");
        SDL_UpdateTrays();
    }
    const std::scoped_lock lock{native_->mutex};
    native_->diagnostics.window_visible = visible;
}

void MacosBackgroundShell::set_notifications_enabled(const bool enabled) noexcept {
    native_->notifications_enabled.store(enabled, std::memory_order_relaxed);
    if (native_->notifications_entry != nullptr) {
        SDL_SetTrayEntryChecked(native_->notifications_entry, enabled);
        SDL_UpdateTrays();
    }
    const std::scoped_lock lock{native_->mutex};
    native_->diagnostics.notifications_enabled = enabled;
}

bool MacosBackgroundShell::notifications_enabled() const noexcept {
    return native_->notifications_enabled.load(std::memory_order_relaxed);
}

bool MacosBackgroundShell::notify(const std::string_view title,
                                  const std::string_view message) noexcept {
    if (!native_->options.use_native_services || !notifications_enabled() ||
        title.empty() || title.size() > 128U || message.empty() ||
        message.size() > 1'024U || title.find('\0') != std::string_view::npos ||
        message.find('\0') != std::string_view::npos) {
        native_->count_notification_drop();
        return false;
    }
    auto authorization = native_->notification_state->authorization.load(
        std::memory_order_relaxed);
    if (authorization == NotificationAuthorization::unknown) {
        if (native_->notification_state->authorization.compare_exchange_strong(
                authorization, NotificationAuthorization::requesting,
                std::memory_order_relaxed)) {
            request_notification_authorization(native_->notification_state);
        }
        native_->count_notification_drop();
        return false;
    }
    if (authorization != NotificationAuthorization::authorized) {
        native_->count_notification_drop();
        return false;
    }

    const auto shared = native_->notification_state;
    {
        const std::scoped_lock lock{shared->mutex};
        if (shared->pending >= maximum_pending_notifications) {
            ++shared->dropped;
            return false;
        }
        ++shared->pending;
    }
    @autoreleasepool {
        NSString* notification_title =
            [[NSString alloc] initWithBytes:title.data()
                                     length:title.size()
                                   encoding:NSUTF8StringEncoding];
        NSString* notification_message =
            [[NSString alloc] initWithBytes:message.data()
                                     length:message.size()
                                   encoding:NSUTF8StringEncoding];
        if (notification_title == nil || notification_message == nil) {
            const std::scoped_lock lock{shared->mutex};
            --shared->pending;
            ++shared->dropped;
            return false;
        }
        auto* content = [[UNMutableNotificationContent alloc] init];
        content.title = notification_title;
        content.body = notification_message;
        UNNotificationRequest* request = [UNNotificationRequest
            requestWithIdentifier:NSUUID.UUID.UUIDString
                          content:content
                          trigger:nil];
        [UNUserNotificationCenter.currentNotificationCenter
            addNotificationRequest:request
             withCompletionHandler:^(NSError* error) {
                 const std::scoped_lock lock{shared->mutex};
                 --shared->pending;
                 if (error == nil) ++shared->sent;
                 else ++shared->dropped;
             }];
    }
    return true;
}

bool MacosBackgroundShell::set_launch_at_login(const bool enabled) noexcept {
    if (!native_->options.use_native_services || !uses_application_bundle()) return false;
    bool matches_requested_state = false;
    @autoreleasepool {
        NSError* error = nil;
        const bool accepted = enabled
            ? [SMAppService.mainAppService registerAndReturnError:&error]
            : [SMAppService.mainAppService unregisterAndReturnError:&error];
        if (!accepted || error != nil) return false;
        matches_requested_state =
            native_->launch_at_login_enabled_unlocked() == enabled;
    }
    if (native_->autostart_entry != nullptr) {
        SDL_SetTrayEntryChecked(native_->autostart_entry,
                                native_->launch_at_login_enabled_unlocked());
        SDL_UpdateTrays();
    }
    return matches_requested_state;
}

bool MacosBackgroundShell::launch_at_login_enabled() const noexcept {
    return native_->launch_at_login_enabled_unlocked();
}

BackgroundShellDiagnostics MacosBackgroundShell::diagnostics() const noexcept {
    BackgroundShellDiagnostics result{};
    {
        const std::scoped_lock lock{native_->mutex};
        result = native_->diagnostics;
    }
    {
        const std::scoped_lock lock{native_->notification_state->mutex};
        result.notifications_sent = native_->notification_state->sent;
        result.notifications_dropped = native_->notification_state->dropped;
    }
    if (native_->notification_state->authorization.load(std::memory_order_relaxed) ==
        NotificationAuthorization::denied) {
        result.notifications_available = false;
    }
    return result;
}

} // namespace blackbox::platform::macos
