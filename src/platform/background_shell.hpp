#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace blackbox::platform {

enum class BackgroundShellCommand : std::uint8_t {
    show_window,
    hide_window,
    capture_incident,
    toggle_recording,
    toggle_launch_at_login,
    toggle_notifications,
    exit_application,
};

enum class BackgroundShellStatus : std::uint8_t {
    recording,
    capturing,
    paused,
    retrying_storage,
    error,
};

enum class BackgroundShellStartResult : std::uint8_t {
    started,
    already_running,
    unavailable,
};

struct BackgroundShellDiagnostics {
    bool running{};
    bool tray_available{};
    bool window_visible{true};
    bool notifications_enabled{true};
    std::uint64_t commands_dispatched{};
    std::uint64_t notifications_sent{};
    std::uint64_t notifications_dropped{};
    std::uint64_t explorer_restarts{};
    std::uint64_t tray_readd_failures{};
    bool session_notifications_available{};
    std::uint64_t session_locks{};
    std::uint64_t session_unlocks{};
};

using BackgroundShellCallback = std::function<void(BackgroundShellCommand)>;

class IBackgroundShell {
public:
    virtual ~IBackgroundShell() = default;

    [[nodiscard]] virtual BackgroundShellStartResult start(
        BackgroundShellCallback callback) = 0;
    virtual void stop() noexcept = 0;
    virtual void set_status(BackgroundShellStatus status) noexcept = 0;
    virtual void set_window_visible(bool visible) noexcept = 0;
    virtual void set_notifications_enabled(bool enabled) noexcept = 0;
    [[nodiscard]] virtual bool notifications_enabled() const noexcept = 0;
    [[nodiscard]] virtual bool notify(std::string_view title,
                                      std::string_view message) noexcept = 0;
    [[nodiscard]] virtual bool set_launch_at_login(bool enabled) noexcept = 0;
    [[nodiscard]] virtual bool launch_at_login_enabled() const noexcept = 0;
    [[nodiscard]] virtual BackgroundShellDiagnostics diagnostics() const noexcept = 0;
};

} // namespace blackbox::platform
