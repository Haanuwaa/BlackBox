#include "platform/windows/windows_global_hotkey_manager.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace platform = blackbox::platform;
namespace windows = blackbox::platform::windows;
using namespace std::chrono_literals;

TEST_CASE("Windows global hotkey registers and unregisters without elevation",
          "[platform][windows][hotkey]") {
    windows::WindowsGlobalHotkeyManager manager;
    auto combination = platform::default_incident_hotkey;
    std::atomic<std::uint32_t> activations{};

    const auto result = manager.register_hotkey(
        combination, [&activations] { ++activations; });
    REQUIRE(result == platform::HotkeyRegistrationResult::registered);
    CHECK(manager.registered());
    REQUIRE(manager.post_activation_for_testing());
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (activations.load() == 0U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(activations.load() == 1U);
    manager.unregister_hotkey();
    CHECK_FALSE(manager.registered());
}

TEST_CASE("default incident hotkey remains Ctrl Shift F12",
          "[platform][hotkey][contract]") {
    CHECK(platform::default_incident_hotkey.control);
    CHECK(platform::default_incident_hotkey.shift);
    CHECK_FALSE(platform::default_incident_hotkey.alt);
    CHECK_FALSE(platform::default_incident_hotkey.windows);
    CHECK(platform::default_incident_hotkey.key == platform::HotkeyKey::f12);
}

TEST_CASE("Windows global hotkey reports a conflict without disturbing its owner",
          "[platform][windows][hotkey][conflict]") {
    windows::WindowsGlobalHotkeyManager owner;
    windows::WindowsGlobalHotkeyManager contender;
    std::atomic<std::uint32_t> owner_activations{};

    REQUIRE(owner.register_hotkey(
                platform::default_incident_hotkey,
                [&owner_activations] { ++owner_activations; }) ==
            platform::HotkeyRegistrationResult::registered);
    CHECK(contender.register_hotkey(
              platform::default_incident_hotkey, [] {}) ==
          platform::HotkeyRegistrationResult::conflict);
    CHECK_FALSE(contender.registered());

    REQUIRE(owner.post_activation_for_testing());
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (owner_activations.load() == 0U &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(owner_activations.load() == 1U);
    owner.unregister_hotkey();
}
