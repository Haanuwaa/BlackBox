#include "platform/macos/macos_accessibility.hpp"
#include "platform/macos/macos_background_shell.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace macos_platform = blackbox::platform::macos;
namespace platform = blackbox::platform;

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("blackbox-macos-shell-" + std::to_string(::getpid()) + '-' +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()));
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

[[nodiscard]] macos_platform::MacosBackgroundShellOptions options_for(
    const TemporaryDirectory& temporary) {
    return {.state_directory = temporary.path() / "state",
            .install_tray_icon = false,
            .use_native_services = false};
}

} // namespace

TEST_CASE("macOS background shell enforces one instance without UI services",
          "[platform][macos][background]") {
    const TemporaryDirectory temporary;
    macos_platform::MacosBackgroundShell first{options_for(temporary)};
    macos_platform::MacosBackgroundShell second{options_for(temporary)};

    REQUIRE(first.start([](platform::BackgroundShellCommand) {}) ==
            platform::BackgroundShellStartResult::started);
    const auto diagnostics = first.diagnostics();
    CHECK(diagnostics.running);
    CHECK_FALSE(diagnostics.tray_available);
    CHECK_FALSE(diagnostics.notifications_available);
    CHECK(diagnostics.launch_at_login_state == platform::LaunchAtLoginState::unsupported);
    CHECK(second.start([](platform::BackgroundShellCommand) {}) ==
          platform::BackgroundShellStartResult::already_running);

    CHECK_FALSE(first.notify("Capture complete", "Saved locally"));
    CHECK(first.diagnostics().notifications_dropped == 1U);
    first.stop();
    CHECK_FALSE(first.diagnostics().running);
    CHECK(second.start([](platform::BackgroundShellCommand) {}) ==
          platform::BackgroundShellStartResult::started);
}

TEST_CASE("macOS accessibility adapter exposes current display preferences",
          "[platform][macos][accessibility]") {
    static_cast<void>(macos_platform::accessibility_preferences());
    SUCCEED("AppKit accessibility preferences were readable without prompting");
}
