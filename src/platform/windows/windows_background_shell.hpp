#pragma once

#include "platform/background_shell.hpp"

#include <memory>
#include <string>

namespace blackbox::platform::windows {

struct WindowsBackgroundShellOptions {
    std::wstring instance_name{L"Local\\BlackBox.BackgroundShell.V1"};
    std::wstring window_class_name{L"BlackBox.BackgroundShell.Window.V1"};
    std::wstring startup_value_name{L"BlackBox"};
    bool install_tray_icon{true};
};

class WindowsBackgroundShell final : public IBackgroundShell {
public:
    explicit WindowsBackgroundShell(WindowsBackgroundShellOptions options = {});
    ~WindowsBackgroundShell() override;

    WindowsBackgroundShell(const WindowsBackgroundShell&) = delete;
    WindowsBackgroundShell& operator=(const WindowsBackgroundShell&) = delete;

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

    [[nodiscard]] bool post_command_for_testing(
        BackgroundShellCommand command) noexcept;
    [[nodiscard]] bool post_taskbar_created_for_testing() noexcept;
    [[nodiscard]] bool post_end_session_for_testing() noexcept;
    [[nodiscard]] bool post_session_change_for_testing(bool locked) noexcept;
    [[nodiscard]] std::uint64_t status_messages_posted_for_testing() const noexcept;

private:
    struct NativeState;
    std::unique_ptr<NativeState> native_;
};

} // namespace blackbox::platform::windows
