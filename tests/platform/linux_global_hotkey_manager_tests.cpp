#include "platform/linux/linux_global_hotkey_manager.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace linux_platform = blackbox::platform::linux;
namespace platform = blackbox::platform;

TEST_CASE("Linux portal accelerator preserves the configured combination",
          "[platform][linux][hotkey][portal]") {
    CHECK(linux_platform::portal_accelerator(
              platform::default_incident_hotkey) == "CTRL+SHIFT+F12");
    CHECK(linux_platform::portal_accelerator(
              platform::HotkeyCombination{platform::HotkeyKey::f3,
                                          false, false, true, true}) ==
          "ALT+LOGO+F3");
}

TEST_CASE("Linux portal hotkey rejects invalid keys before desktop access",
          "[platform][linux][hotkey][portal]") {
    linux_platform::LinuxGlobalHotkeyManager manager;
    auto invalid = platform::default_incident_hotkey;
    invalid.key = static_cast<platform::HotkeyKey>(0U);
    CHECK(manager.register_hotkey(invalid, [] {}) ==
          platform::HotkeyRegistrationResult::invalid_combination);
    CHECK_FALSE(manager.registered());
}

TEST_CASE("Linux portal shortcut lifecycle recovers sessions without overriding removal",
          "[platform][linux][hotkey][portal][lifecycle]") {
    using Event = linux_platform::PortalShortcutEvent;
    using State = linux_platform::PortalShortcutState;

    auto state = State::idle;
    state = linux_platform::portal_shortcut_transition(
        state, Event::session_established);
    CHECK(state == State::active);
    state = linux_platform::portal_shortcut_transition(state, Event::session_lost);
    CHECK(state == State::reconnecting);
    state = linux_platform::portal_shortcut_transition(
        state, Event::session_established);
    CHECK(state == State::active);

    state = linux_platform::portal_shortcut_transition(
        state, Event::shortcut_removed);
    CHECK(state == State::unavailable);
    CHECK(linux_platform::portal_shortcut_transition(
              state, Event::shortcut_restored) == State::active);
    CHECK(linux_platform::portal_shortcut_transition(
              state, Event::stop) == State::idle);
}

TEST_CASE("Linux portal shortcut retry cadence is bounded",
          "[platform][linux][hotkey][portal][lifecycle]") {
    using namespace std::chrono_literals;
    CHECK(linux_platform::portal_reconnect_delay(0U) == 250ms);
    CHECK(linux_platform::portal_reconnect_delay(1U) == 500ms);
    CHECK(linux_platform::portal_reconnect_delay(4U) == 4s);
    CHECK(linux_platform::portal_reconnect_delay(100U) == 4s);
}

TEST_CASE("Linux portal shortcut randomized service-loss model preserves safety invariants",
          "[platform][linux][hotkey][portal][property][service-loss][reconnect]") {
    using Event = linux_platform::PortalShortcutEvent;
    using State = linux_platform::PortalShortcutState;
    constexpr std::array events{Event::session_established, Event::session_lost,
                                Event::shortcut_removed, Event::shortcut_restored, Event::stop};
    std::uint64_t random_state{0xd1b54a32d192ed03ULL};
    auto state = State::idle;
    for (std::size_t iteration = 0U; iteration < 8'192U; ++iteration) {
        random_state ^= random_state << 13U;
        random_state ^= random_state >> 7U;
        random_state ^= random_state << 17U;
        const auto event = events[random_state % events.size()];
        const auto prior = state;
        state = linux_platform::portal_shortcut_transition(state, event);

        if (event == Event::stop) CHECK(state == State::idle);
        if (event == Event::shortcut_removed) CHECK(state == State::unavailable);
        if (event == Event::session_established || event == Event::shortcut_restored) {
            CHECK(state == State::active);
        }
        if (event == Event::session_lost) {
            CHECK(state == (prior == State::idle ? State::idle : State::reconnecting));
        }
        CHECK(static_cast<std::uint8_t>(state) <=
              static_cast<std::uint8_t>(State::unavailable));
    }
}
