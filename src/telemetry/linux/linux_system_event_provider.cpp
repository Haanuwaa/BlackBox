#include "telemetry/linux/linux_system_event_provider.hpp"

#include "telemetry/linux/linux_uevent_parser.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <new>
#include <string_view>

#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(BLACKBOX_HAS_DBUS)
#include <dbus/dbus.h>
#endif

namespace blackbox::telemetry::linux {

core::SystemEvent normalized_linux_sleep_event(const bool sleeping) noexcept {
    core::SystemEvent event{};
    event.source = core::SystemEventSource::power;
    event.kind =
        sleeping ? core::SystemEventKind::suspend : core::SystemEventKind::resume_automatic;
    event.level = core::SystemEventLevel::informational;
    return event;
}

struct LinuxSystemEventProvider::NativeState {
    int socket{-1};
#if defined(BLACKBOX_HAS_DBUS)
    DBusConnection* system_bus{};
#endif
    EventProviderConfiguration configuration{};
    std::uint64_t dropped{};
    bool device_active{};
    bool power_active{};
};

namespace {

[[nodiscard]] EventProviderStatus source_status(const bool device_requested,
                                                const bool device_active,
                                                const bool power_requested,
                                                const bool power_active) noexcept {
    const auto requested =
        static_cast<unsigned>(device_requested) + static_cast<unsigned>(power_requested);
    const auto active = static_cast<unsigned>(device_requested && device_active) +
                        static_cast<unsigned>(power_requested && power_active);
    if (requested == 0U || active == requested)
        return EventProviderStatus::complete;
    return active == 0U ? EventProviderStatus::temporarily_failed : EventProviderStatus::partial;
}

} // namespace

LinuxSystemEventProvider::LinuxSystemEventProvider() noexcept
    : state_{new (std::nothrow) NativeState{}} {}

LinuxSystemEventProvider::~LinuxSystemEventProvider() { stop(); }

EventProviderStatus
LinuxSystemEventProvider::start(const EventProviderConfiguration& configuration) noexcept {
    stop();
    if (state_ == nullptr)
        return EventProviderStatus::temporarily_failed;
    state_->configuration = configuration;
    state_->dropped = 0U;
    if (configuration.device_events) {
        state_->socket =
            ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
        if (state_->socket >= 0) {
            constexpr int receive_buffer_bytes = 256 * 1024;
            static_cast<void>(::setsockopt(state_->socket, SOL_SOCKET, SO_RCVBUF,
                                           &receive_buffer_bytes, sizeof(receive_buffer_bytes)));
            sockaddr_nl address{};
            address.nl_family = AF_NETLINK;
            // Let netlink assign a unique port identifier. A process PID is not a
            // safe socket identity when multiple in-process diagnostic clients exist.
            address.nl_pid = 0U;
            address.nl_groups = 1U;
            if (::bind(state_->socket, reinterpret_cast<const sockaddr*>(&address),
                       sizeof(address)) == 0) {
                state_->device_active = true;
            } else {
                static_cast<void>(::close(state_->socket));
                state_->socket = -1;
            }
        }
    }

#if defined(BLACKBOX_HAS_DBUS)
    if (configuration.power_events) {
        DBusError error;
        dbus_error_init(&error);
        state_->system_bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
        if (state_->system_bus != nullptr) {
            dbus_connection_set_exit_on_disconnect(state_->system_bus, false);
            dbus_bus_add_match(
                state_->system_bus,
                "type='signal',sender='org.freedesktop.login1',"
                "interface='org.freedesktop.login1.Manager',member='PrepareForSleep'",
                &error);
            dbus_connection_flush(state_->system_bus);
            if (!dbus_error_is_set(&error))
                state_->power_active = true;
        }
        dbus_error_free(&error);
    }
#endif

    return source_status(configuration.device_events, state_->device_active,
                         configuration.power_events, state_->power_active);
}

EventProviderPollResult
LinuxSystemEventProvider::poll(const core::MonotonicTimePoint observed_at,
                               const std::span<core::SystemEvent> destination) noexcept {
    if (state_ == nullptr) {
        return {EventProviderStatus::temporarily_failed, 0U, 0U};
    }
    std::size_t count{};

#if defined(BLACKBOX_HAS_DBUS)
    if (state_->power_active && state_->system_bus != nullptr) {
        if (dbus_connection_read_write(state_->system_bus, 0) == 0) {
            state_->power_active = false;
        } else {
            while (count < destination.size()) {
                DBusMessage* message = dbus_connection_pop_message(state_->system_bus);
                if (message == nullptr)
                    break;
                if (dbus_message_is_signal(message, "org.freedesktop.login1.Manager",
                                           "PrepareForSleep")) {
                    dbus_bool_t sleeping{};
                    DBusError error;
                    dbus_error_init(&error);
                    if (dbus_message_get_args(message, &error, DBUS_TYPE_BOOLEAN, &sleeping,
                                              DBUS_TYPE_INVALID) != 0) {
                        auto event = normalized_linux_sleep_event(sleeping != 0);
                        event.observed_at = observed_at;
                        destination[count++] = event;
                    } else {
                        ++state_->dropped;
                    }
                    dbus_error_free(&error);
                }
                dbus_message_unref(message);
            }
        }
    }
#endif

    if (state_->device_active && state_->socket >= 0) {
        std::array<char, 4096U> buffer{};
        while (count < destination.size()) {
            const auto received =
                ::recv(state_->socket, buffer.data(), buffer.size(), MSG_DONTWAIT | MSG_TRUNC);
            if (received < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                if (errno == EINTR)
                    continue;
                if (errno == ENOBUFS) {
                    ++state_->dropped;
                    break;
                }
                state_->device_active = false;
                break;
            }
            if (received == 0)
                break;
            if (static_cast<std::size_t>(received) > buffer.size()) {
                ++state_->dropped;
                continue;
            }
            auto event = normalized_linux_uevent(
                std::string_view{buffer.data(), static_cast<std::size_t>(received)},
                state_->configuration);
            if (!event)
                continue;
            event->observed_at = observed_at;
            destination[count++] = *event;
        }
    }
    return {source_status(state_->configuration.device_events, state_->device_active,
                          state_->configuration.power_events, state_->power_active),
            count, state_->dropped};
}

void LinuxSystemEventProvider::stop() noexcept {
    if (state_ == nullptr)
        return;
    if (state_->socket >= 0) {
        static_cast<void>(::close(state_->socket));
        state_->socket = -1;
    }
#if defined(BLACKBOX_HAS_DBUS)
    if (state_->system_bus != nullptr) {
        dbus_connection_close(state_->system_bus);
        dbus_connection_unref(state_->system_bus);
        state_->system_bus = nullptr;
    }
#endif
    state_->device_active = false;
    state_->power_active = false;
}

EventProviderCapabilities LinuxSystemEventProvider::capabilities() const noexcept {
    EventProviderCapabilities result{};
    result.device_events = true;
#if defined(BLACKBOX_HAS_DBUS)
    result.power_events = true;
#endif
    return result;
}

} // namespace blackbox::telemetry::linux
