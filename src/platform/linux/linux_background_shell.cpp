#include "platform/linux/linux_background_shell.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_tray.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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
    const bool tray_available = native_->create_tray();
    {
        const std::scoped_lock lock{native_->mutex};
        native_->diagnostics = {};
        native_->diagnostics.running = true;
        native_->diagnostics.tray_available = tray_available;
        native_->diagnostics.window_visible = native_->window_visible;
        native_->diagnostics.notifications_enabled = native_->notifications_enabled;
    }
    return BackgroundShellStartResult::started;
}

void LinuxBackgroundShell::stop() noexcept {
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

void LinuxBackgroundShell::set_status(const BackgroundShellStatus status) noexcept {
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

bool LinuxBackgroundShell::notify(std::string_view, std::string_view) noexcept {
    const std::scoped_lock lock{native_->mutex};
    ++native_->diagnostics.notifications_dropped;
    return false;
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
    const std::scoped_lock lock{native_->mutex};
    return native_->diagnostics;
}

} // namespace blackbox::platform::linux
