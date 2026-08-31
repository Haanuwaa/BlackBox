#pragma once

#include "platform/background_shell.hpp"

#include <filesystem>
#include <cstdint>
#include <memory>

namespace blackbox::platform::linux {

enum class LinuxNotificationBackend : std::uint8_t {
    none,
    portal,
    freedesktop,
};

[[nodiscard]] constexpr LinuxNotificationBackend select_notification_backend(
    const std::uint32_t portal_version,
    const bool freedesktop_available) noexcept {
    return portal_version >= 1U
        ? LinuxNotificationBackend::portal
        : freedesktop_available ? LinuxNotificationBackend::freedesktop
                                : LinuxNotificationBackend::none;
}

struct LinuxBackgroundShellOptions {
    std::filesystem::path config_home{};
    std::filesystem::path executable_path{};
    bool install_tray_icon{true};
};

class LinuxBackgroundShell final : public IBackgroundShell {
public:
    explicit LinuxBackgroundShell(LinuxBackgroundShellOptions options = {});
    ~LinuxBackgroundShell() override;

    LinuxBackgroundShell(const LinuxBackgroundShell&) = delete;
    LinuxBackgroundShell& operator=(const LinuxBackgroundShell&) = delete;

    [[nodiscard]] BackgroundShellStartResult start(
        BackgroundShellCallback callback) override;
    void stop() noexcept override;
    void set_status(BackgroundShellStatus status) noexcept override;
    void set_window_visible(bool visible) noexcept override;
    void set_notifications_enabled(bool enabled) noexcept override;
    [[nodiscard]] bool notifications_enabled() const noexcept override;
    [[nodiscard]] bool notify(std::string_view title,
                              std::string_view message) noexcept override;
    [[nodiscard]] bool set_launch_at_login(bool enabled) noexcept override;
    [[nodiscard]] bool launch_at_login_enabled() const noexcept override;
    [[nodiscard]] BackgroundShellDiagnostics diagnostics() const noexcept override;

private:
    struct NativeState;
    std::unique_ptr<NativeState> native_;
};

} // namespace blackbox::platform::linux
