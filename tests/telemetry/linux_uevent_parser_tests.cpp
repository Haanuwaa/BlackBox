#include "telemetry/linux/linux_uevent_parser.hpp"

#include <catch2/catch_test_macros.hpp>

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
    CHECK(event->source == core::SystemEventSource::device);
    CHECK(event->kind == core::SystemEventKind::device_enumerated);
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
    CHECK(event->kind == core::SystemEventKind::device_removed);
    CHECK(event->detail == 6U);

    auto disabled = telemetry::EventProviderConfiguration{};
    disabled.device_events = false;
    CHECK_FALSE(linux_telemetry::normalized_linux_uevent(
                    std::string_view{removal, sizeof(removal) - 1U}, disabled)
                    .has_value());
    constexpr char changed[] = "change@x\0ACTION=change\0SUBSYSTEM=block\0";
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
