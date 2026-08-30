#include "telemetry/linux/linux_system_event_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace core = blackbox::core;
namespace linux_telemetry = blackbox::telemetry::linux;
namespace telemetry = blackbox::telemetry;

TEST_CASE("Linux sleep lifecycle normalizes to privacy-bounded power evidence",
          "[telemetry][linux][events][power]") {
    const auto suspend = linux_telemetry::normalized_linux_sleep_event(true);
    CHECK(suspend.source == core::SystemEventSource::power);
    CHECK(suspend.kind == core::SystemEventKind::suspend);
    CHECK(suspend.level == core::SystemEventLevel::informational);
    CHECK(suspend.native_event_id == 0U);
    CHECK_FALSE(suspend.has_source_utc_time);
    CHECK_FALSE(suspend.has_process_identity);

    const auto resume = linux_telemetry::normalized_linux_sleep_event(false);
    CHECK(resume.kind == core::SystemEventKind::resume_automatic);
}

TEST_CASE("Linux event provider exposes bounded native capabilities",
          "[telemetry][linux][events][native]") {
    linux_telemetry::LinuxSystemEventProvider provider;
    const auto capabilities = provider.capabilities();
    CHECK(capabilities.device_events);
#if defined(BLACKBOX_TEST_HAS_DBUS)
    CHECK(capabilities.power_events);
#else
    CHECK_FALSE(capabilities.power_events);
#endif
    CHECK_FALSE(capabilities.audio_device_events);
    CHECK_FALSE(capabilities.application_events);
    CHECK_FALSE(capabilities.storage_events);

    auto configuration = telemetry::EventProviderConfiguration{};
    configuration.device_events = false;
    configuration.power_events = false;
    REQUIRE(provider.start(configuration) == telemetry::EventProviderStatus::complete);
    std::array<core::SystemEvent, 4U> destination{};
    const auto poll = provider.poll({}, destination);
    CHECK(poll.status == telemetry::EventProviderStatus::complete);
    CHECK(poll.event_count == 0U);
    provider.stop();
}
