#include "telemetry/linux/linux_uevent_parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

namespace core = blackbox::core;
namespace linux_telemetry = blackbox::telemetry::linux;
namespace telemetry = blackbox::telemetry;

TEST_CASE("Linux uevent normalization discards identifiers and retains broad device class",
          "[telemetry][linux][events][privacy]") {
    constexpr char payload[] =
        "add@/devices/pci0000:00/private-path\0"
        "ACTION=add\0"
        "DEVPATH=/devices/pci0000:00/private-path\0"
        "SUBSYSTEM=block\0"
        "DEVNAME=nvme-secret\0";
    const auto event = linux_telemetry::normalized_linux_uevent(
        std::string_view{payload, sizeof(payload) - 1U},
        telemetry::EventProviderConfiguration{});

    REQUIRE(event.has_value());
    CHECK(event->source == core::SystemEventSource::storage);
    CHECK(event->kind == core::SystemEventKind::storage_device_added);
    CHECK(event->detail == 1U);
    CHECK(event->native_event_id == 0U);
    CHECK_FALSE(event->has_source_utc_time);
    CHECK_FALSE(event->has_process_identity);
}

TEST_CASE("Linux uevent normalization accepts removals and fails closed",
          "[telemetry][linux][events]") {
    constexpr char removal[] =
        "remove@x\0ACTION=remove\0SUBSYSTEM=usb\0SERIAL=secret\0";
    const auto event = linux_telemetry::normalized_linux_uevent(
        std::string_view{removal, sizeof(removal) - 1U},
        telemetry::EventProviderConfiguration{});
    REQUIRE(event.has_value());
    CHECK(event->source == core::SystemEventSource::device);
    CHECK(event->kind == core::SystemEventKind::device_removed);
    CHECK(event->detail == 6U);

    auto disabled = telemetry::EventProviderConfiguration{};
    disabled.device_events = false;
    CHECK_FALSE(linux_telemetry::normalized_linux_uevent(
                    std::string_view{removal, sizeof(removal) - 1U}, disabled)
                    .has_value());
    constexpr char changed[] = "change@x\0ACTION=change\0SUBSYSTEM=input\0";
    CHECK_FALSE(linux_telemetry::normalized_linux_uevent(
                    std::string_view{changed, sizeof(changed) - 1U},
                    telemetry::EventProviderConfiguration{})
                    .has_value());
    constexpr char duplicate[] =
        "add@x\0ACTION=add\0ACTION=add\0SUBSYSTEM=block\0";
    CHECK_FALSE(linux_telemetry::normalized_linux_uevent(
                    std::string_view{duplicate, sizeof(duplicate) - 1U},
                    telemetry::EventProviderConfiguration{})
                    .has_value());
}

TEST_CASE("Linux uevent normalization maps portable event classes without identities",
          "[telemetry][linux][events][privacy]") {
    struct Case {
        std::string_view payload;
        core::SystemEventSource source;
        core::SystemEventKind kind;
    };
    constexpr char display[] = "change@secret\0ACTION=change\0SUBSYSTEM=drm\0DEVPATH=secret\0";
    constexpr char network[] = "remove@secret\0ACTION=remove\0SUBSYSTEM=net\0INTERFACE=secret\0";
    constexpr char audio[] = "change@secret\0ACTION=change\0SUBSYSTEM=sound\0DEVNAME=secret\0";
    const std::array cases{
        Case{std::string_view{display, sizeof(display) - 1U},
             core::SystemEventSource::graphics,
             core::SystemEventKind::display_configuration_changed},
        Case{std::string_view{network, sizeof(network) - 1U},
             core::SystemEventSource::network,
             core::SystemEventKind::network_connectivity_changed},
        Case{std::string_view{audio, sizeof(audio) - 1U},
             core::SystemEventSource::audio,
             core::SystemEventKind::audio_endpoint_state_changed},
    };
    for (const auto& test : cases) {
        const auto event = linux_telemetry::normalized_linux_uevent(
            test.payload, telemetry::EventProviderConfiguration{});
        REQUIRE(event.has_value());
        CHECK(event->source == test.source);
        CHECK(event->kind == test.kind);
        CHECK(event->native_event_id == 0U);
        CHECK_FALSE(event->has_source_utc_time);
        CHECK_FALSE(event->has_process_identity);
    }
}
