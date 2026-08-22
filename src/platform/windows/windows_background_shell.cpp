#include "platform/windows/windows_background_shell.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace blackbox::platform::windows {
namespace {

using namespace std::chrono_literals;

constexpr UINT tray_callback_message = WM_APP + 0x41U;
constexpr UINT status_changed_message = WM_APP + 0x42U;
constexpr UINT notification_message = WM_APP + 0x43U;
constexpr UINT duplicate_instance_message = WM_APP + 0x44U;
constexpr UINT test_command_message = WM_APP + 0x45U;
constexpr UINT tray_icon_id = 1U;

constexpr UINT menu_show_hide = 0x5001U;
constexpr UINT menu_capture = 0x5002U;
constexpr UINT menu_pause_resume = 0x5003U;
constexpr UINT menu_launch_at_login = 0x5004U;
constexpr UINT menu_notifications = 0x5005U;
constexpr UINT menu_exit = 0x5006U;

constexpr wchar_t run_key[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

template <std::size_t Size>
void copy_bounded(wchar_t (&destination)[Size], const std::wstring_view source) noexcept {
    const auto count = std::min(source.size(), Size - 1U);
    std::copy_n(source.begin(), count, destination);
    destination[count] = L'\0';
}

[[nodiscard]] std::wstring utf8_to_wide(const std::string_view value,
                                        const std::size_t maximum_characters) {
    if (value.empty()) {
        return {};
    }
    const auto source_size = static_cast<int>(
        std::min<std::size_t>(value.size(), static_cast<std::size_t>(INT_MAX)));
    const auto required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_size, nullptr, 0);
    if (required <= 0) {
        std::wstring fallback;
        fallback.reserve(std::min(value.size(), maximum_characters));
        for (const unsigned char character : value) {
            if (fallback.size() == maximum_characters) {
                break;
            }
            fallback.push_back(static_cast<wchar_t>(character));
        }
        return fallback;
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    static_cast<void>(MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_size,
        result.data(), required));
    if (result.size() > maximum_characters) {
        result.resize(maximum_characters);
    }
    return result;
}

[[nodiscard]] std::wstring executable_command() {
    std::vector<wchar_t> path(32'768U, L'\0');
    const auto length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size()) {
        return {};
    }
    return L"\"" + std::wstring{path.data(), length} + L"\" --background";
}

[[nodiscard]] bool registry_value_exists(const std::wstring& value_name) noexcept {
    if (value_name.empty()) {
        return false;
    }
    DWORD type = 0U;
    DWORD bytes = 0U;
    return RegGetValueW(HKEY_CURRENT_USER, run_key, value_name.c_str(),
                        RRF_RT_REG_SZ, &type, nullptr, &bytes) == ERROR_SUCCESS &&
           type == REG_SZ && bytes > sizeof(wchar_t);
}

[[nodiscard]] bool set_registry_value(const std::wstring& value_name,
                                      const bool enabled) noexcept {
    if (value_name.empty()) {
        return false;
    }
    HKEY key = nullptr;
    const auto opened = RegCreateKeyExW(
        HKEY_CURRENT_USER, run_key, 0U, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (opened != ERROR_SUCCESS || key == nullptr) {
        return false;
    }
    LSTATUS result = ERROR_SUCCESS;
    if (enabled) {
        const auto command = executable_command();
        if (command.empty()) {
            RegCloseKey(key);
            return false;
        }
        const auto bytes = static_cast<DWORD>((command.size() + 1U) * sizeof(wchar_t));
        result = RegSetValueExW(
            key, value_name.c_str(), 0U, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()), bytes);
    } else {
        result = RegDeleteValueW(key, value_name.c_str());
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

[[nodiscard]] constexpr const wchar_t* tooltip_for(
    const BackgroundShellStatus status) noexcept {
    switch (status) {
    case BackgroundShellStatus::recording: return L"BlackBox - recording";
    case BackgroundShellStatus::capturing: return L"BlackBox - capturing incident";
    case BackgroundShellStatus::paused: return L"BlackBox - recording paused";
    case BackgroundShellStatus::retrying_storage:
        return L"BlackBox - retrying incident storage";
    case BackgroundShellStatus::error: return L"BlackBox - attention required";
    }
    return L"BlackBox";
}

[[nodiscard]] HICON icon_for(const BackgroundShellStatus status) noexcept {
    const wchar_t* resource = MAKEINTRESOURCEW(32512);
    switch (status) {
    case BackgroundShellStatus::recording: resource = MAKEINTRESOURCEW(32512); break;
    case BackgroundShellStatus::capturing: resource = MAKEINTRESOURCEW(32516); break;
    case BackgroundShellStatus::paused:
    case BackgroundShellStatus::retrying_storage:
        resource = MAKEINTRESOURCEW(32515);
        break;
    case BackgroundShellStatus::error: resource = MAKEINTRESOURCEW(32513); break;
    }
    return LoadIconW(nullptr, resource);
}

} // namespace

struct WindowsBackgroundShell::NativeState {
    explicit NativeState(WindowsBackgroundShellOptions values)
        : options{std::move(values)} {}

    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) noexcept {
        auto* state = reinterpret_cast<NativeState*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            state = static_cast<NativeState*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(state));
        }
        if (state == nullptr) {
            return DefWindowProcW(window, message, wparam, lparam);
        }
        return state->handle_message(window, message, wparam, lparam);
    }

    [[nodiscard]] bool add_tray_icon() noexcept {
        if (!options.install_tray_icon || window.load() == nullptr) {
            tray_installed = false;
            return false;
        }
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window.load();
        data.uID = tray_icon_id;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = tray_callback_message;
        const auto current_status = status.load();
        data.hIcon = icon_for(current_status);
        copy_bounded(data.szTip, std::wstring_view{tooltip_for(current_status)});
        tray_installed = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        if (tray_installed) {
            data.uVersion = NOTIFYICON_VERSION_4;
            static_cast<void>(Shell_NotifyIconW(NIM_SETVERSION, &data));
        }
        {
            const std::scoped_lock lock{diagnostics_mutex};
            diagnostics.tray_available = tray_installed;
        }
        return tray_installed;
    }

    void remove_tray_icon() noexcept {
        if (!tray_installed || window.load() == nullptr) {
            return;
        }
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window.load();
        data.uID = tray_icon_id;
        static_cast<void>(Shell_NotifyIconW(NIM_DELETE, &data));
        tray_installed = false;
        const std::scoped_lock lock{diagnostics_mutex};
        diagnostics.tray_available = false;
    }

    void update_tray_icon() noexcept {
        if (!tray_installed || window.load() == nullptr) {
            return;
        }
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window.load();
        data.uID = tray_icon_id;
        data.uFlags = NIF_ICON | NIF_TIP;
        const auto current_status = status.load();
        data.hIcon = icon_for(current_status);
        copy_bounded(data.szTip, std::wstring_view{tooltip_for(current_status)});
        static_cast<void>(Shell_NotifyIconW(NIM_MODIFY, &data));
    }

    void dispatch(const BackgroundShellCommand command) noexcept {
        try {
            callback(command);
            const std::scoped_lock lock{diagnostics_mutex};
            ++diagnostics.commands_dispatched;
        } catch (...) {
            // No application callback failure crosses the native message thread.
        }
    }

    void show_menu() noexcept {
        const auto menu = CreatePopupMenu();
        if (menu == nullptr) {
            return;
        }
        const bool visible = window_visible.load();
        static_cast<void>(AppendMenuW(
            menu, MF_STRING, menu_show_hide,
            visible ? L"Hide BlackBox" : L"Show BlackBox"));
        static_cast<void>(AppendMenuW(menu, MF_STRING, menu_capture,
                                      L"Capture incident"));
        const bool paused = status.load() == BackgroundShellStatus::paused;
        static_cast<void>(AppendMenuW(menu, MF_STRING, menu_pause_resume,
                                      paused ? L"Resume recording" : L"Pause recording"));
        static_cast<void>(AppendMenuW(menu, MF_SEPARATOR, 0U, nullptr));
        static_cast<void>(AppendMenuW(
            menu, MF_STRING | (registry_value_exists(options.startup_value_name)
                                   ? MF_CHECKED : MF_UNCHECKED),
            menu_launch_at_login, L"Start with Windows"));
        static_cast<void>(AppendMenuW(
            menu, MF_STRING | (notifications_enabled.load()
                                   ? MF_CHECKED : MF_UNCHECKED),
            menu_notifications, L"Notifications"));
        static_cast<void>(AppendMenuW(menu, MF_SEPARATOR, 0U, nullptr));
        static_cast<void>(AppendMenuW(menu, MF_STRING, menu_exit, L"Exit BlackBox"));

        POINT cursor{};
        static_cast<void>(GetCursorPos(&cursor));
        static_cast<void>(SetForegroundWindow(window.load()));
        const auto selected = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            cursor.x, cursor.y, 0, window.load(), nullptr);
        DestroyMenu(menu);
        switch (selected) {
        case menu_show_hide:
            dispatch(visible ? BackgroundShellCommand::hide_window
                             : BackgroundShellCommand::show_window);
            break;
        case menu_capture: dispatch(BackgroundShellCommand::capture_incident); break;
        case menu_pause_resume: dispatch(BackgroundShellCommand::toggle_recording); break;
        case menu_launch_at_login:
            dispatch(BackgroundShellCommand::toggle_launch_at_login);
            break;
        case menu_notifications:
            dispatch(BackgroundShellCommand::toggle_notifications);
            break;
        case menu_exit: dispatch(BackgroundShellCommand::exit_application); break;
        default: break;
        }
    }

    void show_notification() noexcept {
        std::wstring title;
        std::wstring body;
        {
            const std::scoped_lock lock{notification_mutex};
            if (!notification_pending) {
                return;
            }
            title = std::move(notification_title);
            body = std::move(notification_body);
            notification_pending = false;
        }
        if (!tray_installed || !notifications_enabled.load()) {
            const std::scoped_lock lock{diagnostics_mutex};
            ++diagnostics.notifications_dropped;
            return;
        }
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window.load();
        data.uID = tray_icon_id;
        data.uFlags = NIF_INFO;
        data.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
        copy_bounded(data.szInfoTitle, title);
        copy_bounded(data.szInfo, body);
        const bool sent = Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE;
        const std::scoped_lock lock{diagnostics_mutex};
        if (sent) {
            ++diagnostics.notifications_sent;
        } else {
            ++diagnostics.notifications_dropped;
        }
    }

    [[nodiscard]] LRESULT handle_message(HWND native_window, const UINT message,
                                         const WPARAM wparam,
                                         const LPARAM lparam) noexcept {
        if (message == taskbar_created_message) {
            {
                const std::scoped_lock lock{diagnostics_mutex};
                ++diagnostics.explorer_restarts;
            }
            if (!add_tray_icon() && options.install_tray_icon) {
                const std::scoped_lock lock{diagnostics_mutex};
                ++diagnostics.tray_readd_failures;
            }
            return 0;
        }
        switch (message) {
        case tray_callback_message: {
            const auto event = static_cast<UINT>(LOWORD(lparam));
            if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
                show_menu();
            } else if (event == NIN_SELECT || event == NIN_KEYSELECT ||
                       event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK) {
                dispatch(BackgroundShellCommand::show_window);
            }
            return 0;
        }
        case status_changed_message:
            update_tray_icon();
            return 0;
        case notification_message:
            show_notification();
            return 0;
        case duplicate_instance_message:
            dispatch(BackgroundShellCommand::show_window);
            return 0;
        case test_command_message:
            if (wparam <= static_cast<WPARAM>(BackgroundShellCommand::exit_application)) {
                dispatch(static_cast<BackgroundShellCommand>(wparam));
            }
            return 0;
        case WM_WTSSESSION_CHANGE: {
            const std::scoped_lock lock{diagnostics_mutex};
            if (wparam == WTS_SESSION_LOCK) {
                ++diagnostics.session_locks;
            } else if (wparam == WTS_SESSION_UNLOCK) {
                ++diagnostics.session_unlocks;
            }
            return 0;
        }
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wparam != FALSE) {
                dispatch(BackgroundShellCommand::exit_application);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(native_window);
            return 0;
        case WM_DESTROY:
            if (session_notifications_registered) {
                static_cast<void>(WTSUnRegisterSessionNotification(native_window));
                session_notifications_registered = false;
            }
            remove_tray_icon();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(native_window, message, wparam, lparam);
        }
    }

    void thread_main() noexcept {
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &NativeState::window_proc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.lpszClassName = options.window_class_name.c_str();
        class_registered = RegisterClassExW(&window_class) != 0U;
        if (!class_registered && GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            class_registered = true;
        }
        if (class_registered) {
            window.store(CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                options.window_class_name.c_str(), L"", WS_POPUP,
                0, 0, 0, 0, nullptr, nullptr, instance, this));
        }
        taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
        if (window.load() != nullptr) {
            static_cast<void>(add_tray_icon());
            session_notifications_registered =
                WTSRegisterSessionNotification(window.load(), NOTIFY_FOR_THIS_SESSION) != FALSE;
        }
        {
            const std::scoped_lock lock{ready_mutex};
            initialized = true;
            initialization_succeeded = window.load() != nullptr;
            const std::scoped_lock diagnostics_lock{diagnostics_mutex};
            diagnostics.running = initialization_succeeded;
            diagnostics.window_visible = window_visible.load();
            diagnostics.notifications_enabled = notifications_enabled.load();
            diagnostics.session_notifications_available =
                session_notifications_registered;
        }
        ready_condition.notify_all();
        if (!initialization_succeeded) {
            return;
        }

        MSG message{};
        while (GetMessageW(&message, nullptr, 0U, 0U) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        window.store(nullptr);
        if (class_registered) {
            static_cast<void>(UnregisterClassW(options.window_class_name.c_str(), instance));
        }
        const std::scoped_lock lock{diagnostics_mutex};
        diagnostics.running = false;
        diagnostics.tray_available = false;
    }

    WindowsBackgroundShellOptions options{};
    BackgroundShellCallback callback{};
    std::jthread worker{};
    mutable std::mutex lifecycle_mutex{};
    HANDLE instance_mutex{};
    std::atomic<HWND> window{};
    bool class_registered{};
    bool tray_installed{};
    bool session_notifications_registered{};
    UINT taskbar_created_message{};

    std::mutex ready_mutex{};
    std::condition_variable ready_condition{};
    bool initialized{};
    bool initialization_succeeded{};

    std::atomic<BackgroundShellStatus> status{BackgroundShellStatus::recording};
    std::atomic<std::uint64_t> status_messages_posted{};
    std::atomic<bool> window_visible{true};
    std::atomic<bool> notifications_enabled{true};

    mutable std::mutex notification_mutex{};
    std::wstring notification_title{};
    std::wstring notification_body{};
    bool notification_pending{};

    mutable std::mutex diagnostics_mutex{};
    BackgroundShellDiagnostics diagnostics{};
};

WindowsBackgroundShell::WindowsBackgroundShell(WindowsBackgroundShellOptions options)
    : native_{std::make_unique<NativeState>(std::move(options))} {}

WindowsBackgroundShell::~WindowsBackgroundShell() {
    stop();
}

BackgroundShellStartResult WindowsBackgroundShell::start(
    BackgroundShellCallback callback) {
    stop();
    if (!callback || native_->options.instance_name.empty() ||
        native_->options.window_class_name.empty()) {
        return BackgroundShellStartResult::unavailable;
    }

    const std::scoped_lock lifecycle_lock{native_->lifecycle_mutex};
    native_->instance_mutex = CreateMutexW(
        nullptr, FALSE, native_->options.instance_name.c_str());
    if (native_->instance_mutex == nullptr) {
        return BackgroundShellStartResult::unavailable;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(native_->instance_mutex);
        native_->instance_mutex = nullptr;
        for (std::uint32_t attempt = 0U; attempt < 50U; ++attempt) {
            if (const auto existing = FindWindowW(
                    native_->options.window_class_name.c_str(), nullptr);
                existing != nullptr) {
                static_cast<void>(PostMessageW(
                    existing, duplicate_instance_message, 0U, 0));
                break;
            }
            std::this_thread::sleep_for(10ms);
        }
        return BackgroundShellStartResult::already_running;
    }

    native_->callback = std::move(callback);
    native_->initialized = false;
    native_->initialization_succeeded = false;
    native_->worker = std::jthread{[state = native_.get()] {
        state->thread_main();
    }};
    {
        std::unique_lock ready_lock{native_->ready_mutex};
        native_->ready_condition.wait(ready_lock, [this] {
            return native_->initialized;
        });
    }
    if (!native_->initialization_succeeded) {
        if (native_->worker.joinable()) {
            native_->worker.join();
        }
        CloseHandle(native_->instance_mutex);
        native_->instance_mutex = nullptr;
        return BackgroundShellStartResult::unavailable;
    }
    return BackgroundShellStartResult::started;
}

void WindowsBackgroundShell::stop() noexcept {
    const std::scoped_lock lifecycle_lock{native_->lifecycle_mutex};
    if (const auto window = native_->window.load(); window != nullptr) {
        static_cast<void>(PostMessageW(window, WM_CLOSE, 0U, 0));
    }
    if (native_->worker.joinable()) {
        try {
            native_->worker.join();
        } catch (...) {
            // Shutdown remains non-throwing.
        }
    }
    if (native_->instance_mutex != nullptr) {
        CloseHandle(native_->instance_mutex);
        native_->instance_mutex = nullptr;
    }
    native_->callback = {};
}

void WindowsBackgroundShell::set_status(
    const BackgroundShellStatus status) noexcept {
    const auto previous = native_->status.exchange(status);
    if (previous == status) {
        return;
    }
    const auto window = native_->window.load();
    if (window == nullptr) {
        return;
    }
    if (PostMessageW(window, status_changed_message, 0U, 0) != FALSE) {
        ++native_->status_messages_posted;
        return;
    }
    auto expected = status;
    static_cast<void>(native_->status.compare_exchange_strong(expected, previous));
}

void WindowsBackgroundShell::set_window_visible(const bool visible) noexcept {
    native_->window_visible.store(visible);
    const std::scoped_lock lock{native_->diagnostics_mutex};
    native_->diagnostics.window_visible = visible;
}

void WindowsBackgroundShell::set_notifications_enabled(const bool enabled) noexcept {
    native_->notifications_enabled.store(enabled);
    const std::scoped_lock lock{native_->diagnostics_mutex};
    native_->diagnostics.notifications_enabled = enabled;
}

bool WindowsBackgroundShell::notifications_enabled() const noexcept {
    return native_->notifications_enabled.load();
}

bool WindowsBackgroundShell::notify(const std::string_view title,
                                    const std::string_view message) noexcept {
    if (!native_->notifications_enabled.load() ||
        native_->window.load() == nullptr) {
        return false;
    }
    bool replaced_pending_notification = false;
    bool post_failed = false;
    try {
        const std::scoped_lock lock{native_->notification_mutex};
        replaced_pending_notification = native_->notification_pending;
        native_->notification_title = utf8_to_wide(title, 63U);
        native_->notification_body = utf8_to_wide(message, 255U);
        if (!replaced_pending_notification) {
            native_->notification_pending = true;
            if (PostMessageW(native_->window.load(), notification_message, 0U, 0) ==
                FALSE) {
                native_->notification_pending = false;
                post_failed = true;
            }
        }
    } catch (...) {
        return false;
    }
    if (replaced_pending_notification || post_failed) {
        const std::scoped_lock lock{native_->diagnostics_mutex};
        ++native_->diagnostics.notifications_dropped;
    }
    return !post_failed;
}

bool WindowsBackgroundShell::set_launch_at_login(const bool enabled) noexcept {
    return set_registry_value(native_->options.startup_value_name, enabled);
}

bool WindowsBackgroundShell::launch_at_login_enabled() const noexcept {
    return registry_value_exists(native_->options.startup_value_name);
}

BackgroundShellDiagnostics WindowsBackgroundShell::diagnostics() const noexcept {
    const std::scoped_lock lock{native_->diagnostics_mutex};
    return native_->diagnostics;
}

bool WindowsBackgroundShell::post_command_for_testing(
    const BackgroundShellCommand command) noexcept {
    const auto window = native_->window.load();
    return window != nullptr &&
           PostMessageW(window, test_command_message,
                        static_cast<WPARAM>(command), 0) != FALSE;
}

bool WindowsBackgroundShell::post_taskbar_created_for_testing() noexcept {
    const auto window = native_->window.load();
    return window != nullptr && native_->taskbar_created_message != 0U &&
           PostMessageW(window, native_->taskbar_created_message, 0U, 0) != FALSE;
}

bool WindowsBackgroundShell::post_end_session_for_testing() noexcept {
    const auto window = native_->window.load();
    return window != nullptr &&
           PostMessageW(window, WM_ENDSESSION, TRUE, ENDSESSION_CLOSEAPP) != FALSE;
}

bool WindowsBackgroundShell::post_session_change_for_testing(
    const bool locked) noexcept {
    const auto window = native_->window.load();
    return window != nullptr &&
           PostMessageW(window, WM_WTSSESSION_CHANGE,
                        locked ? WTS_SESSION_LOCK : WTS_SESSION_UNLOCK, 0) != FALSE;
}

std::uint64_t WindowsBackgroundShell::status_messages_posted_for_testing() const
    noexcept {
    return native_->status_messages_posted.load();
}

} // namespace blackbox::platform::windows
