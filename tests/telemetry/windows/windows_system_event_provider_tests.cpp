#include "telemetry/windows/windows_system_event_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace core = blackbox::core;
namespace telemetry = blackbox::telemetry;
namespace windows = blackbox::telemetry::windows;

TEST_CASE("Windows system event provider exposes independently gated sources",
          "[telemetry][windows][events][capabilities]") {
    windows::WindowsSystemEventProvider provider;
    const auto capabilities = provider.capabilities();
    CHECK(capabilities.power_events);
    CHECK(capabilities.device_events);
    CHECK(capabilities.audio_device_events);
    CHECK(capabilities.service_events);
    CHECK(capabilities.security_events);
    CHECK(capabilities.update_events);
    CHECK(capabilities.application_events);
    CHECK(capabilities.network_events);
    CHECK(capabilities.graphics_events);
    CHECK(capabilities.storage_events);
}

TEST_CASE("Windows event allowlist maps disk retry without device payload",
          "[telemetry][windows][events][storage][privacy]") {
    const auto retry =
        windows::normalized_windows_event_kind(core::SystemEventSource::storage, 153U);
    REQUIRE(retry.has_value());
    CHECK(*retry == core::SystemEventKind::storage_io_retry);
    CHECK_FALSE(
        windows::normalized_windows_event_kind(core::SystemEventSource::storage, 154U).has_value());
    CHECK_FALSE(
        windows::normalized_windows_event_kind(core::SystemEventSource::device, 153U).has_value());
}

TEST_CASE("Windows event allowlist maps application crash without retaining "
          "fault payload",
          "[telemetry][windows][events][application][crash][privacy]") {
    const auto crash =
        windows::normalized_windows_event_kind(core::SystemEventSource::application, 1000U);
    REQUIRE(crash.has_value());
    CHECK(*crash == core::SystemEventKind::application_crash);
    const auto hang =
        windows::normalized_windows_event_kind(core::SystemEventSource::application, 1002U);
    REQUIRE(hang.has_value());
    CHECK(*hang == core::SystemEventKind::application_hang);
    CHECK_FALSE(windows::normalized_windows_event_kind(core::SystemEventSource::application, 1001U)
                    .has_value());
    const auto defender =
        windows::normalized_windows_event_kind(core::SystemEventSource::security, 1000U);
    REQUIRE(defender.has_value());
    CHECK(*defender == core::SystemEventKind::security_scan_started);
}

TEST_CASE("Windows event allowlist maps display recovery without driver payload",
          "[telemetry][windows][events][graphics][privacy]") {
    const auto recovery =
        windows::normalized_windows_event_kind(core::SystemEventSource::graphics, 4101U);
    REQUIRE(recovery.has_value());
    CHECK(*recovery == core::SystemEventKind::display_driver_recovery);
    CHECK_FALSE(windows::normalized_windows_event_kind(core::SystemEventSource::graphics, 4102U)
                    .has_value());
    CHECK_FALSE(windows::normalized_windows_event_kind(core::SystemEventSource::application, 4101U)
                    .has_value());
}

TEST_CASE("Windows event allowlist maps DNS timeout without retaining event payload",
          "[telemetry][windows][events][dns][privacy]") {
    const auto dns =
        windows::normalized_windows_event_kind(core::SystemEventSource::network, 1014U);
    REQUIRE(dns.has_value());
    CHECK(*dns == core::SystemEventKind::dns_resolution_timeout);
    CHECK_FALSE(windows::normalized_windows_event_kind(core::SystemEventSource::network, 1015U)
                    .has_value());
    CHECK_FALSE(windows::normalized_windows_event_kind(core::SystemEventSource::application, 1014U)
                    .has_value());
}

TEST_CASE("Windows system event provider is inert when every source is disabled",
          "[telemetry][windows][events][privacy]") {
    windows::WindowsSystemEventProvider provider;
    const telemetry::EventProviderConfiguration disabled{false, false, false, false, false,
                                                         false, false, false, false, false};
    CHECK(provider.start(disabled) == telemetry::EventProviderStatus::complete);
    std::array<core::SystemEvent, 8U> events{};
    const auto result = provider.poll(core::MonotonicClock::now(), events);
    CHECK(result.status == telemetry::EventProviderStatus::complete);
    CHECK(result.event_count == 0U);
    CHECK(result.native_events_dropped == 0U);
    provider.stop();
}
