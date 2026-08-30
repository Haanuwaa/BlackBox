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
#if defined(BLACKBOX_HAS_DBUS)
    CHECK(capabilities.power_events);
    CHECK(capabilities.service_events);
#else
    CHECK_FALSE(capabilities.power_events);
    CHECK_FALSE(capabilities.service_events);
#endif
    CHECK(capabilities.audio_device_events);
    CHECK(capabilities.storage_events);
    CHECK(capabilities.network_events);
    CHECK(capabilities.graphics_events);
#if defined(BLACKBOX_HAS_SYSTEMD_JOURNAL)
    CHECK(capabilities.application_events);
#else
    CHECK_FALSE(capabilities.application_events);
#endif

    auto configuration = telemetry::EventProviderConfiguration{};
    configuration.device_events = false;
    configuration.power_events = false;
    configuration.audio_device_events = false;
    configuration.service_events = false;
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

TEST_CASE("Linux service job events discard unit identity and preserve result class",
          "[telemetry][linux][events][privacy]") {
    const auto completed = linux_telemetry::normalized_linux_service_job_event("done");
    CHECK(completed.source == core::SystemEventSource::service_control_manager);
    CHECK(completed.kind == core::SystemEventKind::service_state_changed);
    CHECK(completed.level == core::SystemEventLevel::informational);
    CHECK(completed.detail == 0U);
    CHECK_FALSE(completed.has_process_identity);

    const auto failed = linux_telemetry::normalized_linux_service_job_event("failed");
    CHECK(failed.level == core::SystemEventLevel::warning);
    CHECK(failed.detail == 1U);
    CHECK(failed.native_event_id == 0U);
}

TEST_CASE("Linux coredump notification is identity-free crash evidence",
          "[telemetry][linux][events][privacy]") {
    const auto event = linux_telemetry::normalized_linux_application_crash_event();
    CHECK(event.source == core::SystemEventSource::application);
    CHECK(event.kind == core::SystemEventKind::application_crash);
    CHECK(event.level == core::SystemEventLevel::error);
    CHECK_FALSE(event.has_source_utc_time);
    CHECK_FALSE(event.has_process_identity);
}
