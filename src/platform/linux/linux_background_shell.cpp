#include "platform/linux/linux_background_shell.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_tray.h>
#include <dbus/dbus.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <thread>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

namespace blackbox::platform::linux {
namespace {

[[nodiscard]] std::filesystem::path default_config_home() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        return xdg;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path{home} / ".config";
    }
    return {};
}

[[nodiscard]] std::filesystem::path default_executable_path() {
    std::array<char, 4096U> buffer{};
    const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
    if (length <= 0 || static_cast<std::size_t>(length) >= buffer.size()) return {};
    return std::filesystem::path{std::string{buffer.data(), static_cast<std::size_t>(length)}};
}

[[nodiscard]] std::string quote_desktop_exec(const std::filesystem::path& executable) {
    const auto utf8 = executable.u8string();
    std::string result{"\""};
    result.reserve(utf8.size() + 16U);
    for (const char8_t value : utf8) {
        const auto character = static_cast<char>(value);
        if (character == '\\' || character == '"' || character == '`' || character == '$') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    result += "\" --start-hidden";
    return result;
}

[[nodiscard]] bool is_plain_directory(const std::filesystem::path& path) noexcept {
    std::error_code issue{};
    const auto status = std::filesystem::symlink_status(path, issue);
    return !issue && std::filesystem::is_directory(status) &&
           !std::filesystem::is_symlink(status);
}

[[nodiscard]] bool is_plain_file_or_missing(const std::filesystem::path& path) noexcept {
    std::error_code issue{};
    const auto status = std::filesystem::symlink_status(path, issue);
    if (issue == std::errc::no_such_file_or_directory) return true;
    return !issue && std::filesystem::is_regular_file(status) &&
           !std::filesystem::is_symlink(status);
}

[[nodiscard]] bool write_all(const int descriptor, const std::string_view contents) noexcept {
    std::size_t offset{};
    while (offset < contents.size()) {
        const auto written = ::write(descriptor, contents.data() + offset,
                                     contents.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return ::fsync(descriptor) == 0;
}

struct PendingNotification {
    std::string title{};
    std::string message{};
};

constexpr char portal_service[] = "org.freedesktop.portal.Desktop";
constexpr char portal_path[] = "/org/freedesktop/portal/desktop";
constexpr char portal_notification_interface[] =
    "org.freedesktop.portal.Notification";
constexpr char portal_background_interface[] =
    "org.freedesktop.portal.Background";
constexpr char properties_interface[] = "org.freedesktop.DBus.Properties";
constexpr char freedesktop_notification_service[] =
    "org.freedesktop.Notifications";

[[nodiscard]] bool append_string(DBusMessageIter& destination,
                                 const char* value) noexcept {
    return dbus_message_iter_append_basic(
               &destination, DBUS_TYPE_STRING, &value) != FALSE;
}

[[nodiscard]] bool append_string_variant(DBusMessageIter& options,
                                         const char* key,
                                         const char* value) noexcept {
    DBusMessageIter entry{};
    DBusMessageIter variant{};
    return dbus_message_iter_open_container(
               &options, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) != FALSE &&
           append_string(entry, key) &&
           dbus_message_iter_open_container(
               &entry, DBUS_TYPE_VARIANT, DBUS_TYPE_STRING_AS_STRING,
               &variant) != FALSE &&
           append_string(variant, value) &&
           dbus_message_iter_close_container(&entry, &variant) != FALSE &&
           dbus_message_iter_close_container(&options, &entry) != FALSE;
}

[[nodiscard]] bool service_available(DBusConnection* connection,
                                     const char* service) noexcept {
    if (connection == nullptr) return false;
    DBusError error{};
    dbus_error_init(&error);
    const bool available = dbus_bus_name_has_owner(
        connection, service, &error) != FALSE;
    if (dbus_error_is_set(&error)) dbus_error_free(&error);
    return available;
}

[[nodiscard]] std::uint32_t interface_version(
    DBusConnection* connection, const char* interface_name) noexcept {
    DBusMessage* request = dbus_message_new_method_call(
        portal_service, portal_path, properties_interface, "Get");
    if (request == nullptr) return 0U;
    DBusMessageIter arguments{};
    dbus_message_iter_init_append(request, &arguments);
    const char* property = "version";
    const bool valid = append_string(arguments, interface_name) &&
                       append_string(arguments, property);
    DBusMessage* reply = valid ? dbus_connection_send_with_reply_and_block(
                                     connection, request, 500, nullptr)
                               : nullptr;
    dbus_message_unref(request);
    if (reply == nullptr ||
        dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        if (reply != nullptr) dbus_message_unref(reply);
        return 0U;
    }
    DBusMessageIter value{};
    bool decoded = dbus_message_iter_init(reply, &value) != FALSE;
    if (decoded && dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_VARIANT) {
        DBusMessageIter nested{};
        dbus_message_iter_recurse(&value, &nested);
        value = nested;
    }
    dbus_uint32_t version{};
    decoded = decoded &&
              dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_UINT32;
    if (decoded) dbus_message_iter_get_basic(&value, &version);
    dbus_message_unref(reply);
    return decoded ? static_cast<std::uint32_t>(version) : 0U;
}

[[nodiscard]] bool send_freedesktop_notification(
    DBusConnection* connection,
    const PendingNotification& notification) noexcept {
    if (connection == nullptr) return false;
    DBusMessage* request = dbus_message_new_method_call(
        freedesktop_notification_service, "/org/freedesktop/Notifications",
        freedesktop_notification_service, "Notify");
    if (request == nullptr) return false;

    DBusMessageIter values{};
    dbus_message_iter_init_append(request, &values);
    const char* application = "BlackBox";
    const char* icon = "io.github.Haanuwaa.BlackBox";
    const char* title = notification.title.c_str();
    const char* body = notification.message.c_str();
    dbus_uint32_t replaces_id{};
    dbus_int32_t timeout_milliseconds = 5'000;
    bool valid = append_string(values, application) &&
                 dbus_message_iter_append_basic(
                     &values, DBUS_TYPE_UINT32, &replaces_id) != FALSE &&
                 append_string(values, icon) && append_string(values, title) &&
                 append_string(values, body);

    DBusMessageIter actions{};
    if (valid) {
        valid = dbus_message_iter_open_container(
                    &values, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING,
                    &actions) != FALSE;
        if (valid) {
            valid = dbus_message_iter_close_container(&values, &actions) != FALSE;
        }
    }
    DBusMessageIter hints{};
    if (valid) {
        valid = dbus_message_iter_open_container(
                    &values, DBUS_TYPE_ARRAY, "{sv}", &hints) != FALSE;
        if (valid) {
            valid = dbus_message_iter_close_container(&values, &hints) != FALSE;
        }
    }
    if (valid) {
        valid = dbus_message_iter_append_basic(
                    &values, DBUS_TYPE_INT32, &timeout_milliseconds) != FALSE;
    }

    DBusMessage* reply = valid
        ? dbus_connection_send_with_reply_and_block(connection, request, 750, nullptr)
        : nullptr;
    dbus_message_unref(request);
    if (reply == nullptr) return false;
    const bool success = dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_ERROR;
    dbus_message_unref(reply);
    return success;
}

[[nodiscard]] bool send_portal_notification(
    DBusConnection* connection, const PendingNotification& notification,
    const std::uint64_t sequence) noexcept {
    if (connection == nullptr) return false;
    DBusMessage* request = dbus_message_new_method_call(
        portal_service, portal_path, portal_notification_interface,
        "AddNotification");
    if (request == nullptr) return false;
    DBusMessageIter arguments{};
    DBusMessageIter options{};
    dbus_message_iter_init_append(request, &arguments);
    const auto identifier = std::string{"blackbox-"} + std::to_string(sequence);
    const char* id = identifier.c_str();
    const char* title = notification.title.c_str();
    const char* body = notification.message.c_str();
    bool valid = append_string(arguments, id) &&
        dbus_message_iter_open_container(
            &arguments, DBUS_TYPE_ARRAY, "{sv}", &options) != FALSE &&
        append_string_variant(options, "title", title) &&
        append_string_variant(options, "body", body) &&
        dbus_message_iter_close_container(&arguments, &options) != FALSE;
    DBusMessage* reply = valid ? dbus_connection_send_with_reply_and_block(
                                     connection, request, 750, nullptr)
                               : nullptr;
    dbus_message_unref(request);
    if (reply == nullptr) return false;
    const bool success = dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_ERROR;
    dbus_message_unref(reply);
    return success;
}

[[nodiscard]] const char* portal_status_text(
    const BackgroundShellStatus status) noexcept {
    switch (status) {
    case BackgroundShellStatus::recording:
        return "Recording system performance locally";
    case BackgroundShellStatus::capturing:
        return "Capturing an incident locally";
    case BackgroundShellStatus::paused:
        return "Recording paused";
    case BackgroundShellStatus::retrying_storage:
        return "Recording; retrying the local archive";
    case BackgroundShellStatus::error:
        return "Recording needs attention";
    }
    return "Running";
}

[[nodiscard]] bool send_portal_background_status(
    DBusConnection* connection, const BackgroundShellStatus status) noexcept {
    if (connection == nullptr) return false;
    DBusMessage* request = dbus_message_new_method_call(
        portal_service, portal_path, portal_background_interface, "SetStatus");
    if (request == nullptr) return false;
    DBusMessageIter arguments{};
    DBusMessageIter options{};
    dbus_message_iter_init_append(request, &arguments);
    const char* text = portal_status_text(status);
    bool valid = dbus_message_iter_open_container(
                     &arguments, DBUS_TYPE_ARRAY, "{sv}", &options) != FALSE &&
                 append_string_variant(options, "status", text) &&
                 dbus_message_iter_close_container(&arguments, &options) != FALSE;
    DBusMessage* reply = valid ? dbus_connection_send_with_reply_and_block(
                                     connection, request, 500, nullptr)
                               : nullptr;
    dbus_message_unref(request);
    if (reply == nullptr) return false;
    const bool success = dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_ERROR;
    dbus_message_unref(reply);
    return success;
}

} // namespace

struct LinuxBackgroundShell::NativeState {
    explicit NativeState(LinuxBackgroundShellOptions value) : options{std::move(value)} {
        if (options.config_home.empty()) options.config_home = default_config_home();
        if (options.executable_path.empty()) options.executable_path = default_executable_path();
    }

    [[nodiscard]] std::filesystem::path state_directory() const {
        return options.config_home / "blackbox";
    }

    [[nodiscard]] std::filesystem::path autostart_directory() const {
        return options.config_home / "autostart";
    }

    [[nodiscard]] std::filesystem::path autostart_path() const {
        return autostart_directory() / "blackbox.desktop";
    }

    [[nodiscard]] std::string autostart_contents() const {
        return "[Desktop Entry]\n"
               "Type=Application\n"
               "Version=1.0\n"
               "Name=BlackBox\n"
               "Comment=Keep a bounded local history for incident capture\n"
               "Exec=" + quote_desktop_exec(options.executable_path) + "\n"
               "Terminal=false\n"
               "StartupNotify=false\n"
               "X-GNOME-Autostart-enabled=true\n";
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

    void close_desktop_connection() noexcept {
        if (notification_connection != nullptr) {
            dbus_connection_close(notification_connection);
            dbus_connection_unref(notification_connection);
            notification_connection = nullptr;
        }
        notification_backend = LinuxNotificationBackend::none;
        portal_background_available = false;
    }

    [[nodiscard]] bool refresh_desktop_services() noexcept {
        const auto previous_backend = notification_backend;
        const bool previous_background = portal_background_available.load();
        bool reopened{};
        if (notification_connection == nullptr ||
            dbus_connection_get_is_connected(notification_connection) == FALSE) {
            close_desktop_connection();
            DBusError error{};
            dbus_error_init(&error);
            notification_connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
            if (dbus_error_is_set(&error)) dbus_error_free(&error);
            if (notification_connection == nullptr) {
                const std::scoped_lock lock{mutex};
                diagnostics.notifications_available = false;
                diagnostics.portal_notifications_active = false;
                diagnostics.background_status_available = false;
                if (desktop_services_initialized &&
                    (previous_backend != LinuxNotificationBackend::none ||
                     previous_background)) {
                    ++diagnostics.desktop_service_reconnects;
                }
                desktop_services_initialized = true;
                return false;
            }
            dbus_connection_set_exit_on_disconnect(notification_connection, FALSE);
            reopened = desktop_services_initialized;
        }

        const bool portal = service_available(notification_connection, portal_service);
        const auto notification_version = portal
            ? interface_version(notification_connection,
                                portal_notification_interface)
            : 0U;
        const auto background_version = portal
            ? interface_version(notification_connection, portal_background_interface)
            : 0U;
        const bool direct = service_available(
            notification_connection, freedesktop_notification_service);
        notification_backend = select_notification_backend(
            notification_version, direct);
        portal_background_available.store(background_version >= 1U);

        const std::scoped_lock lock{mutex};
        diagnostics.notifications_available =
            notification_backend != LinuxNotificationBackend::none;
        diagnostics.portal_notifications_active =
            notification_backend == LinuxNotificationBackend::portal;
        diagnostics.background_status_available = portal_background_available.load();
        if (desktop_services_initialized &&
            (reopened || previous_backend != notification_backend ||
             previous_background != portal_background_available.load())) {
            ++diagnostics.desktop_service_reconnects;
        }
        if (!previous_background && portal_background_available.load()) {
            status_dirty = true;
        }
        desktop_services_initialized = true;
        return diagnostics.notifications_available ||
               diagnostics.background_status_available;
    }

    [[nodiscard]] bool start_notifications() noexcept {
        static_cast<void>(dbus_threads_init_default());
        desktop_services_initialized = false;
        const bool available = refresh_desktop_services();
        status_dirty = portal_background_available.load();
        notification_worker = std::jthread{
            [this](const std::stop_token stop_token) { notification_loop(stop_token); }};
        return available && notification_backend != LinuxNotificationBackend::none;
    }

    void stop_notifications() noexcept {
        if (notification_worker.joinable()) {
            notification_worker.request_stop();
            notification_condition.notify_all();
            notification_worker.join();
        }
        close_desktop_connection();
        const std::scoped_lock lock{mutex};
        diagnostics.notifications_dropped += notification_size;
        notification_head = 0U;
        notification_size = 0U;
        status_dirty = false;
        diagnostics.notifications_available = false;
        diagnostics.portal_notifications_active = false;
        diagnostics.background_status_available = false;
    }

    void notification_loop(const std::stop_token stop_token) noexcept {
        while (!stop_token.stop_requested()) {
            PendingNotification pending{};
            bool has_notification{};
            bool has_status{};
            BackgroundShellStatus pending_status{};
            {
                std::unique_lock lock{mutex};
                const auto refresh_interval =
                    diagnostics.notifications_available ||
                            diagnostics.background_status_available
                        ? std::chrono::seconds{5}
                        : std::chrono::seconds{1};
                static_cast<void>(notification_condition.wait_for(
                    lock, stop_token, refresh_interval,
                    [this] { return notification_size != 0U || status_dirty; }));
                if (stop_token.stop_requested()) break;
                if (notification_size != 0U) {
                    pending = std::move(notifications[notification_head]);
                    notification_head =
                        (notification_head + 1U) % notifications.size();
                    --notification_size;
                    has_notification = true;
                }
                if (status_dirty) {
                    pending_status = status;
                    status_dirty = false;
                    has_status = true;
                }
            }

            static_cast<void>(refresh_desktop_services());
            bool status_sent{};
            if (has_status && portal_background_available.load()) {
                status_sent = send_portal_background_status(
                    notification_connection, pending_status);
            }
            bool sent{};
            if (has_notification) {
                if (notification_backend == LinuxNotificationBackend::portal) {
                    sent = send_portal_notification(
                        notification_connection, pending, ++notification_sequence);
                    if (!sent && service_available(
                            notification_connection,
                            freedesktop_notification_service)) {
                        sent = send_freedesktop_notification(
                            notification_connection, pending);
                    }
                } else if (notification_backend == LinuxNotificationBackend::freedesktop) {
                    sent = send_freedesktop_notification(
                        notification_connection, pending);
                }
            }
            const std::scoped_lock lock{mutex};
            if (has_notification) {
                if (sent) ++diagnostics.notifications_sent;
                else ++diagnostics.notifications_dropped;
            }
            if (status_sent) ++diagnostics.background_status_updates;
        }
    }

    [[nodiscard]] bool queue_notification(std::string_view title,
                                          std::string_view message) noexcept {
        if (!notifications_enabled || title.empty() || title.size() > 128U || message.empty() ||
            message.size() > 1'024U || title.find('\0') != std::string_view::npos ||
            message.find('\0') != std::string_view::npos) {
            const std::scoped_lock lock{mutex};
            ++diagnostics.notifications_dropped;
            return false;
        }
        try {
            const std::scoped_lock lock{mutex};
            if (!diagnostics.notifications_available ||
                notification_size == notifications.size()) {
                ++diagnostics.notifications_dropped;
                return false;
            }
            const auto tail = (notification_head + notification_size) % notifications.size();
            notifications[tail] = PendingNotification{std::string{title}, std::string{message}};
            ++notification_size;
        } catch (...) {
            const std::scoped_lock lock{mutex};
            ++diagnostics.notifications_dropped;
            return false;
        }
        notification_condition.notify_one();
        return true;
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
        if (!options.install_tray_icon || (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0U) {
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
        if (show_hide_entry == nullptr || capture_entry == nullptr || pause_entry == nullptr ||
            autostart_entry == nullptr || notifications_entry == nullptr || exit_entry == nullptr) {
            SDL_DestroyTray(tray);
            tray = nullptr;
            return false;
        }
        SDL_SetTrayEntryChecked(autostart_entry, launch_at_login_enabled_unlocked());
        SDL_SetTrayEntryChecked(notifications_entry, notifications_enabled);
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
        const auto path = autostart_path();
        if (!is_plain_file_or_missing(path)) return false;
        std::ifstream input{path, std::ios::binary};
        if (!input.is_open()) return false;
        const std::string contents{std::istreambuf_iterator<char>{input},
                                   std::istreambuf_iterator<char>{}};
        return contents == autostart_contents();
    }

    LinuxBackgroundShellOptions options{};
    BackgroundShellCallback callback{};
    mutable std::mutex mutex{};
    std::condition_variable_any notification_condition{};
    BackgroundShellDiagnostics diagnostics{};
    int lock_descriptor{-1};
    bool window_visible{true};
    bool notifications_enabled{true};
    BackgroundShellStatus status{BackgroundShellStatus::recording};
    SDL_Tray* tray{};
    SDL_TrayEntry* show_hide_entry{};
    SDL_TrayEntry* capture_entry{};
    SDL_TrayEntry* pause_entry{};
    SDL_TrayEntry* autostart_entry{};
    SDL_TrayEntry* notifications_entry{};
    SDL_TrayEntry* exit_entry{};
    DBusConnection* notification_connection{};
    LinuxNotificationBackend notification_backend{
        LinuxNotificationBackend::none};
    std::atomic<bool> portal_background_available{};
    bool desktop_services_initialized{};
    bool status_dirty{};
    std::uint64_t notification_sequence{};
    std::jthread notification_worker{};
    std::array<PendingNotification, 8U> notifications{};
    std::size_t notification_head{};
    std::size_t notification_size{};
};

LinuxBackgroundShell::LinuxBackgroundShell(LinuxBackgroundShellOptions options)
    : native_{std::make_unique<NativeState>(std::move(options))} {}

LinuxBackgroundShell::~LinuxBackgroundShell() { stop(); }

BackgroundShellStartResult LinuxBackgroundShell::start(BackgroundShellCallback callback) {
    stop();
    if (!callback || native_->options.config_home.empty() ||
        native_->options.executable_path.empty() || !native_->options.executable_path.is_absolute()) {
        return BackgroundShellStartResult::unavailable;
    }
    std::error_code issue{};
    std::filesystem::create_directories(native_->state_directory(), issue);
    if (issue || !is_plain_directory(native_->state_directory())) {
        return BackgroundShellStartResult::unavailable;
    }
    const auto lock_path = native_->state_directory() / "instance.lock";
    native_->lock_descriptor = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
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
    {
        const std::scoped_lock lock{native_->mutex};
        native_->diagnostics = {};
    }
    const bool tray_available = native_->create_tray();
    const bool notifications_available = native_->start_notifications();
    {
        const std::scoped_lock lock{native_->mutex};
        native_->diagnostics.running = true;
        native_->diagnostics.tray_available = tray_available;
        native_->diagnostics.window_visible = native_->window_visible;
        native_->diagnostics.notifications_available = notifications_available;
        native_->diagnostics.notifications_enabled = native_->notifications_enabled;
    }
    return BackgroundShellStartResult::started;
}

void LinuxBackgroundShell::stop() noexcept {
    native_->destroy_tray();
    native_->stop_notifications();
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

void LinuxBackgroundShell::set_status(const BackgroundShellStatus status) noexcept {
    {
        const std::scoped_lock lock{native_->mutex};
        if (native_->status != status) {
            native_->status = status;
            native_->status_dirty = native_->portal_background_available.load();
        }
    }
    native_->notification_condition.notify_one();
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

void LinuxBackgroundShell::set_window_visible(const bool visible) noexcept {
    native_->window_visible = visible;
    if (native_->show_hide_entry != nullptr) {
        SDL_SetTrayEntryLabel(native_->show_hide_entry,
                              visible ? "Hide BlackBox" : "Show BlackBox");
        SDL_UpdateTrays();
    }
    const std::scoped_lock lock{native_->mutex};
    native_->diagnostics.window_visible = visible;
}

void LinuxBackgroundShell::set_notifications_enabled(const bool enabled) noexcept {
    native_->notifications_enabled = enabled;
    if (native_->notifications_entry != nullptr) {
        SDL_SetTrayEntryChecked(native_->notifications_entry, enabled);
        SDL_UpdateTrays();
    }
    const std::scoped_lock lock{native_->mutex};
    native_->diagnostics.notifications_enabled = enabled;
}

bool LinuxBackgroundShell::notifications_enabled() const noexcept {
    return native_->notifications_enabled;
}

bool LinuxBackgroundShell::notify(const std::string_view title,
                                  const std::string_view message) noexcept {
    return native_->queue_notification(title, message);
}

bool LinuxBackgroundShell::set_launch_at_login(const bool enabled) noexcept {
    const auto destination = native_->autostart_path();
    if (!is_plain_file_or_missing(destination)) return false;
    if (!enabled) {
        if (!native_->launch_at_login_enabled_unlocked()) return false;
        std::error_code issue{};
        const bool removed = std::filesystem::remove(destination, issue);
        if (!removed || issue) return false;
    } else {
        std::error_code existence_issue{};
        if (std::filesystem::exists(destination, existence_issue)) {
            return !existence_issue && native_->launch_at_login_enabled_unlocked();
        }
        if (existence_issue) return false;
        std::error_code issue{};
        std::filesystem::create_directories(native_->autostart_directory(), issue);
        if (issue || !is_plain_directory(native_->autostart_directory())) return false;
        const auto temporary = destination.string() + ".tmp-" + std::to_string(::getpid());
        const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                                         O_NOFOLLOW,
                                      S_IRUSR | S_IWUSR);
        if (descriptor < 0) return false;
        const auto contents = native_->autostart_contents();
        const bool written = write_all(descriptor, contents);
        const bool closed = ::close(descriptor) == 0;
        if (!written || !closed || ::rename(temporary.c_str(), destination.c_str()) != 0) {
            static_cast<void>(::unlink(temporary.c_str()));
            return false;
        }
    }
    if (native_->autostart_entry != nullptr) {
        SDL_SetTrayEntryChecked(native_->autostart_entry, enabled);
        SDL_UpdateTrays();
    }
    return true;
}

bool LinuxBackgroundShell::launch_at_login_enabled() const noexcept {
    return native_->launch_at_login_enabled_unlocked();
}

BackgroundShellDiagnostics LinuxBackgroundShell::diagnostics() const noexcept {
    BackgroundShellDiagnostics result{};
    {
        const std::scoped_lock lock{native_->mutex};
        result = native_->diagnostics;
    }
    result.launch_at_login_state = native_->launch_at_login_enabled_unlocked()
                                       ? LaunchAtLoginState::enabled
                                       : LaunchAtLoginState::disabled;
    return result;
}

} // namespace blackbox::platform::linux
