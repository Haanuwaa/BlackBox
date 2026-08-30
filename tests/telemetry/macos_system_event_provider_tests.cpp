#include "telemetry/macos/macos_system_event_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace core = blackbox::core;
namespace macos = blackbox::telemetry::macos;
namespace telemetry = blackbox::telemetry;

TEST_CASE("macOS media notifications normalize to identifier-free storage evidence",
          "[telemetry][macos][events][privacy]") {
    const auto added = macos::normalized_macos_media_event(true);
    CHECK(added.source == core::SystemEventSource::storage);
    CHECK(added.kind == core::SystemEventKind::storage_device_added);
    CHECK(added.detail == 1U);
    CHECK(added.native_event_id == 0U);
    CHECK_FALSE(added.has_source_utc_time);
    CHECK_FALSE(added.has_process_identity);

    const auto removed = macos::normalized_macos_media_event(false);
    CHECK(removed.kind == core::SystemEventKind::storage_device_removed);
}

TEST_CASE("macOS sleep lifecycle normalizes to privacy-bounded power evidence",
          "[telemetry][macos][events][power]") {
    const auto suspend = macos::normalized_macos_sleep_event(true);
    CHECK(suspend.source == core::SystemEventSource::power);
    CHECK(suspend.kind == core::SystemEventKind::suspend);
    CHECK(suspend.level == core::SystemEventLevel::informational);
    CHECK(suspend.native_event_id == 0U);
    CHECK_FALSE(suspend.has_source_utc_time);
    CHECK_FALSE(suspend.has_process_identity);

    const auto resume = macos::normalized_macos_sleep_event(false);
    CHECK(resume.kind == core::SystemEventKind::resume_automatic);
}

TEST_CASE("macOS event provider exposes bounded native capabilities",
          "[telemetry][macos][events][native]") {
    macos::MacosSystemEventProvider provider;
    const auto capabilities = provider.capabilities();
    CHECK_FALSE(capabilities.device_events);
    CHECK(capabilities.power_events);
    CHECK(capabilities.audio_device_events);
    CHECK(capabilities.application_events);
    CHECK(capabilities.network_events);
    CHECK(capabilities.graphics_events);
    CHECK(capabilities.storage_events);
    CHECK_FALSE(capabilities.service_events);

    auto configuration = telemetry::EventProviderConfiguration{};
    configuration.device_events = false;
    configuration.power_events = false;
    configuration.audio_device_events = false;
    configuration.application_events = false;
    configuration.network_events = false;
    configuration.graphics_events = false;
    configuration.storage_events = false;
    REQUIRE(provider.start(configuration) == telemetry::EventProviderStatus::complete);
    std::array<core::SystemEvent, 4U> destination{};
    const auto poll = provider.poll({}, destination);
    CHECK(poll.status == telemetry::EventProviderStatus::complete);
    CHECK(poll.event_count == 0U);
    provider.stop();
}

TEST_CASE("macOS context events are portable and identity-free",
          "[telemetry][macos][events][privacy]") {
    const std::array events{
        macos::normalized_macos_application_event(true),
        macos::normalized_macos_application_event(false),
        macos::normalized_macos_audio_event(false),
        macos::normalized_macos_audio_event(true),
        macos::normalized_macos_display_event(),
        macos::normalized_macos_network_event(),
    };
    CHECK(events[0].kind == core::SystemEventKind::application_started);
    CHECK(events[1].kind == core::SystemEventKind::application_terminated);
    CHECK(events[2].kind == core::SystemEventKind::audio_endpoint_state_changed);
    CHECK(events[3].kind == core::SystemEventKind::audio_default_changed);
    CHECK(events[4].kind == core::SystemEventKind::display_configuration_changed);
    CHECK(events[5].kind == core::SystemEventKind::network_connectivity_changed);
    for (const auto& event : events) {
        CHECK(event.native_event_id == 0U);
        CHECK(event.detail == 0U);
        CHECK_FALSE(event.has_source_utc_time);
        CHECK_FALSE(event.has_process_identity);
    }
}
