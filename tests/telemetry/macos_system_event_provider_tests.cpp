#include "telemetry/macos/macos_system_event_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace core = blackbox::core;
namespace macos = blackbox::telemetry::macos;
namespace telemetry = blackbox::telemetry;

TEST_CASE("macOS media notifications normalize to identifier-free device evidence",
          "[telemetry][macos][events][privacy]") {
    const auto added = macos::normalized_macos_media_event(true);
    CHECK(added.source == core::SystemEventSource::device);
    CHECK(added.kind == core::SystemEventKind::device_enumerated);
    CHECK(added.detail == 1U);
    CHECK(added.native_event_id == 0U);
    CHECK_FALSE(added.has_source_utc_time);
    CHECK_FALSE(added.has_process_identity);

    const auto removed = macos::normalized_macos_media_event(false);
    CHECK(removed.kind == core::SystemEventKind::device_removed);
}

TEST_CASE("macOS event provider exposes a bounded device-only capability",
          "[telemetry][macos][events][native]") {
    macos::MacosSystemEventProvider provider;
    const auto capabilities = provider.capabilities();
    CHECK(capabilities.device_events);
    CHECK_FALSE(capabilities.audio_device_events);
    CHECK_FALSE(capabilities.application_events);
    CHECK_FALSE(capabilities.storage_events);

    auto configuration = telemetry::EventProviderConfiguration{};
    configuration.device_events = false;
    REQUIRE(provider.start(configuration) == telemetry::EventProviderStatus::complete);
    std::array<core::SystemEvent, 4U> destination{};
    const auto poll = provider.poll({}, destination);
    CHECK(poll.status == telemetry::EventProviderStatus::complete);
    CHECK(poll.event_count == 0U);
    provider.stop();
}
