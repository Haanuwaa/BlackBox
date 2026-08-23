#pragma once

#include "platform/background_shell.hpp"

#include <filesystem>
#include <memory>

namespace blackbox::platform::macos {

struct MacosBackgroundShellOptions {
    std::filesystem::path state_directory{};
    bool install_tray_icon{true};
    bool use_native_services{true};
};

class MacosBackgroundShell final : public IBackgroundShell {
public:
    explicit MacosBackgroundShell(MacosBackgroundShellOptions options = {});
    ~MacosBackgroundShell() override;

    MacosBackgroundShell(const MacosBackgroundShell&) = delete;
    MacosBackgroundShell& operator=(const MacosBackgroundShell&) = delete;

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

} // namespace blackbox::platform::macos
