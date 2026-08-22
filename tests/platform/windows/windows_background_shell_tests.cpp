#include "platform/windows/windows_background_shell.hpp"

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace platform = blackbox::platform;
namespace windows = blackbox::platform::windows;
using namespace std::chrono_literals;

namespace {

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate,
                              const std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

[[nodiscard]] windows::WindowsBackgroundShellOptions options_for(
    const std::wstring& suffix) {
    const auto identity = std::to_wstring(GetCurrentProcessId()) + L"." + suffix;
    return {
        L"Local\\BlackBox.BackgroundShell.Test." + identity,
        L"BlackBox.BackgroundShell.Test.Window." + identity,
        L"BlackBoxTestStartup." + identity,
        false,
    };
}

} // namespace

TEST_CASE("Windows background shell dispatches bounded lifecycle commands and restarts",
          "[platform][windows][background]") {
    windows::WindowsBackgroundShell shell{options_for(L"commands")};
    std::atomic<std::uint64_t> commands{};
    REQUIRE(shell.start([&commands](platform::BackgroundShellCommand) {
                ++commands;
            }) == platform::BackgroundShellStartResult::started);

    shell.set_status(platform::BackgroundShellStatus::capturing);
    shell.set_window_visible(false);
    shell.set_notifications_enabled(false);
    CHECK_FALSE(shell.notifications_enabled());
    CHECK_FALSE(shell.notify("Hidden", "Notifications are disabled"));
    REQUIRE(shell.post_command_for_testing(
        platform::BackgroundShellCommand::capture_incident));
    REQUIRE(shell.post_command_for_testing(
        platform::BackgroundShellCommand::toggle_recording));
    REQUIRE(wait_until([&commands] { return commands.load() == 2U; }));

    REQUIRE(shell.post_taskbar_created_for_testing());
    REQUIRE(wait_until([&shell] {
        return shell.diagnostics().explorer_restarts == 1U;
    }));
    REQUIRE(shell.post_session_change_for_testing(true));
    REQUIRE(shell.post_session_change_for_testing(false));
    REQUIRE(wait_until([&shell] {
        const auto current = shell.diagnostics();
        return current.session_locks == 1U && current.session_unlocks == 1U;
    }));
    const auto diagnostics = shell.diagnostics();
    CHECK(diagnostics.running);
    CHECK_FALSE(diagnostics.tray_available);
    CHECK_FALSE(diagnostics.window_visible);
    CHECK(diagnostics.commands_dispatched == 2U);
    CHECK(diagnostics.session_locks == 1U);
    CHECK(diagnostics.session_unlocks == 1U);

    shell.stop();
    CHECK_FALSE(shell.diagnostics().running);
    CHECK_FALSE(shell.post_command_for_testing(
        platform::BackgroundShellCommand::show_window));
}

TEST_CASE("second Windows background shell activates the existing instance",
          "[platform][windows][background][single-instance]") {
    const auto options = options_for(L"single-instance");
    windows::WindowsBackgroundShell first{options};
    windows::WindowsBackgroundShell second{options};
    std::atomic<std::uint64_t> show_requests{};
    REQUIRE(first.start([&show_requests](const platform::BackgroundShellCommand command) {
                if (command == platform::BackgroundShellCommand::show_window) {
                    ++show_requests;
                }
            }) == platform::BackgroundShellStartResult::started);
    CHECK(second.start([](platform::BackgroundShellCommand) {}) ==
          platform::BackgroundShellStartResult::already_running);
    REQUIRE(wait_until([&show_requests] { return show_requests.load() == 1U; }));
    second.stop();
    first.stop();
}

TEST_CASE("launch-at-login uses a removable current-user value",
          "[platform][windows][background][startup]") {
    windows::WindowsBackgroundShell shell{options_for(L"startup")};
    REQUIRE(shell.set_launch_at_login(false));
    CHECK_FALSE(shell.launch_at_login_enabled());
    REQUIRE(shell.set_launch_at_login(true));
    CHECK(shell.launch_at_login_enabled());
    REQUIRE(shell.set_launch_at_login(false));
    CHECK_FALSE(shell.launch_at_login_enabled());
}

TEST_CASE("background shell shutdown tolerates queued commands",
          "[platform][windows][background][shutdown]") {
    windows::WindowsBackgroundShell shell{options_for(L"shutdown")};
    REQUIRE(shell.start([](platform::BackgroundShellCommand) {}) ==
            platform::BackgroundShellStartResult::started);
    std::jthread producer{[&shell](const std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            static_cast<void>(shell.post_command_for_testing(
                platform::BackgroundShellCommand::show_window));
        }
    }};
    std::this_thread::sleep_for(10ms);
    shell.stop();
    producer.request_stop();
    producer.join();
    CHECK_FALSE(shell.diagnostics().running);

    REQUIRE(shell.start([](platform::BackgroundShellCommand) {}) ==
            platform::BackgroundShellStartResult::started);
    shell.stop();
}

TEST_CASE("Windows background shell coalesces a notification burst",
          "[platform][windows][background][notifications]") {
    windows::WindowsBackgroundShell shell{options_for(L"notification-burst")};
    std::atomic<bool> callback_entered{};
    std::atomic<bool> release_callback{};
    REQUIRE(shell.start([&](platform::BackgroundShellCommand) {
                callback_entered.store(true);
                while (!release_callback.load()) {
                    std::this_thread::yield();
                }
            }) == platform::BackgroundShellStartResult::started);

    REQUIRE(shell.post_command_for_testing(
        platform::BackgroundShellCommand::show_window));
    const bool entered = wait_until([&callback_entered] {
        return callback_entered.load();
    });
    if (!entered) {
        release_callback.store(true);
        shell.stop();
        FAIL("shell callback did not enter its deterministic blocking window");
    }

    CHECK(shell.notify("First", "First pending notification"));
    CHECK(shell.notify("Second", "Replacement notification"));
    CHECK(shell.notify("Third", "Final replacement notification"));
    CHECK(shell.diagnostics().notifications_dropped == 2U);

    release_callback.store(true);
    REQUIRE(wait_until([&shell] {
        return shell.diagnostics().notifications_dropped == 3U;
    }));
    std::this_thread::sleep_for(25ms);
    const auto diagnostics = shell.diagnostics();
    CHECK(diagnostics.notifications_sent == 0U);
    CHECK(diagnostics.notifications_dropped == 3U);
    shell.stop();
}

TEST_CASE("Windows background shell posts only status transitions",
          "[platform][windows][background][status]") {
    windows::WindowsBackgroundShell shell{options_for(L"status-transitions")};
    shell.set_status(platform::BackgroundShellStatus::capturing);
    CHECK(shell.status_messages_posted_for_testing() == 0U);
    REQUIRE(shell.start([](platform::BackgroundShellCommand) {}) ==
            platform::BackgroundShellStartResult::started);

    for (std::size_t index = 0; index < 128U; ++index) {
        shell.set_status(platform::BackgroundShellStatus::capturing);
    }
    CHECK(shell.status_messages_posted_for_testing() == 0U);

    shell.set_status(platform::BackgroundShellStatus::recording);
    CHECK(shell.status_messages_posted_for_testing() == 1U);
    for (std::size_t index = 0; index < 128U; ++index) {
        shell.set_status(platform::BackgroundShellStatus::recording);
    }
    CHECK(shell.status_messages_posted_for_testing() == 1U);

    shell.set_status(platform::BackgroundShellStatus::paused);
    CHECK(shell.status_messages_posted_for_testing() == 2U);
    shell.stop();
}

TEST_CASE("Windows session shutdown requests application exit on the shell thread",
          "[platform][windows][background][shutdown]") {
    windows::WindowsBackgroundShell shell{options_for(L"end-session")};
    std::atomic<bool> exit_requested{};
    REQUIRE(shell.start([&exit_requested](const platform::BackgroundShellCommand command) {
                if (command == platform::BackgroundShellCommand::exit_application) {
                    exit_requested.store(true);
                }
            }) == platform::BackgroundShellStartResult::started);

    REQUIRE(shell.post_end_session_for_testing());
    REQUIRE(wait_until([&exit_requested] { return exit_requested.load(); }));
    shell.stop();
}
