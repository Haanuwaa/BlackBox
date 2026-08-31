#include "platform/linux/linux_background_shell.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace linux_platform = blackbox::platform::linux;
namespace platform = blackbox::platform;

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("blackbox-linux-shell-" + std::to_string(::getpid()) + '-' +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code issue{};
        std::filesystem::remove_all(path_, issue);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_{};
};

[[nodiscard]] linux_platform::LinuxBackgroundShellOptions options_for(
    const TemporaryDirectory& temporary) {
    return {.config_home = temporary.path() / "config",
            .executable_path = "/opt/Black Box/blackbox",
            .install_tray_icon = false};
}

} // namespace

TEST_CASE("Linux background shell enforces one instance without requiring a tray",
          "[platform][linux][background]") {
    const TemporaryDirectory temporary;
    linux_platform::LinuxBackgroundShell first{options_for(temporary)};
    linux_platform::LinuxBackgroundShell second{options_for(temporary)};

    REQUIRE(first.start([](platform::BackgroundShellCommand) {}) ==
            platform::BackgroundShellStartResult::started);
    const auto diagnostics = first.diagnostics();
    CHECK(diagnostics.running);
    CHECK_FALSE(diagnostics.tray_available);
    CHECK(second.start([](platform::BackgroundShellCommand) {}) ==
          platform::BackgroundShellStartResult::already_running);

    first.stop();
    CHECK_FALSE(first.diagnostics().running);
    CHECK(second.start([](platform::BackgroundShellCommand) {}) ==
          platform::BackgroundShellStartResult::started);
}

TEST_CASE("Linux autostart is an exact owned XDG desktop entry",
          "[platform][linux][background][autostart]") {
    const TemporaryDirectory temporary;
    linux_platform::LinuxBackgroundShell shell{options_for(temporary)};
    REQUIRE(shell.start([](platform::BackgroundShellCommand) {}) ==
            platform::BackgroundShellStartResult::started);
    CHECK_FALSE(shell.launch_at_login_enabled());
    REQUIRE(shell.set_launch_at_login(true));
    CHECK(shell.launch_at_login_enabled());

    const auto desktop = temporary.path() / "config" / "autostart" / "blackbox.desktop";
    std::ifstream input{desktop, std::ios::binary};
    const std::string contents{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
    CHECK(contents.find("format=") == std::string::npos);
    CHECK(contents.find("Exec=\"/opt/Black Box/blackbox\" --start-hidden\n") !=
          std::string::npos);
    CHECK(contents.find("Terminal=false\n") != std::string::npos);

    {
        std::ofstream tamper{desktop, std::ios::binary | std::ios::trunc};
        tamper << "[Desktop Entry]\nName=Someone else's entry\n";
    }
    CHECK_FALSE(shell.launch_at_login_enabled());
    CHECK_FALSE(shell.set_launch_at_login(false));
    CHECK_FALSE(shell.set_launch_at_login(true));
    CHECK(std::filesystem::exists(desktop));

    std::filesystem::remove(desktop);
    REQUIRE(shell.set_launch_at_login(true));
    REQUIRE(shell.set_launch_at_login(false));
    CHECK_FALSE(std::filesystem::exists(desktop));
}

TEST_CASE("Linux preview reports unavailable session-bus notifications honestly",
          "[platform][linux][background][notification]") {
    const TemporaryDirectory temporary;
    linux_platform::LinuxBackgroundShell shell{options_for(temporary)};
    REQUIRE(shell.start([](platform::BackgroundShellCommand) {}) ==
            platform::BackgroundShellStartResult::started);
    shell.set_notifications_enabled(true);
    const auto available = shell.diagnostics().notifications_available;
    const auto accepted = shell.notify("Capture complete", "Saved locally");
    CHECK(accepted == available);
    if (!available) CHECK(shell.diagnostics().notifications_dropped == 1U);
    shell.set_notifications_enabled(false);
    CHECK_FALSE(shell.notifications_enabled());
    CHECK_FALSE(shell.notify("Capture complete", "Saved locally"));
}

TEST_CASE("Linux notification integration prefers the permission-bounded portal",
          "[platform][linux][background][notification][portal]") {
    using Backend = linux_platform::LinuxNotificationBackend;
    CHECK(linux_platform::select_notification_backend(2U, true) ==
          Backend::portal);
    CHECK(linux_platform::select_notification_backend(1U, false) ==
          Backend::portal);
    CHECK(linux_platform::select_notification_backend(0U, true) ==
          Backend::freedesktop);
    CHECK(linux_platform::select_notification_backend(0U, false) ==
          Backend::none);
}
