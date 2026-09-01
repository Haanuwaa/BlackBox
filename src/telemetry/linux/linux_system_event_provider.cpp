#include "telemetry/linux/linux_system_event_provider.hpp"

#include "telemetry/linux/linux_uevent_parser.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <new>
#include <string_view>
#include <utility>

#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(BLACKBOX_HAS_DBUS)
#include <dbus/dbus.h>
#endif
#if defined(BLACKBOX_HAS_SYSTEMD_JOURNAL)
#include <systemd/sd-journal.h>
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

core::SystemEvent normalized_linux_service_job_event(const std::string_view result) noexcept {
    core::SystemEvent event{};
    event.source = core::SystemEventSource::service_manager;
    event.kind = core::SystemEventKind::service_state_changed;
    event.level = result == "done" || result == "skipped" ? core::SystemEventLevel::informational
                                                          : core::SystemEventLevel::warning;
    // Only the bounded result class is retained. Unit name, object path, job
    // identifier, and message text are deliberately discarded.
    event.detail = event.level == core::SystemEventLevel::informational ? 0U : 1U;
    return event;
}

core::SystemEvent normalized_linux_application_crash_event() noexcept {
    core::SystemEvent event{};
    event.source = core::SystemEventSource::application;
    event.kind = core::SystemEventKind::application_crash;
    event.level = core::SystemEventLevel::error;
    return event;
}

struct LinuxSystemEventProvider::NativeState {
    int socket{-1};
#if defined(BLACKBOX_HAS_DBUS)
    DBusConnection* system_bus{};
#endif
#if defined(BLACKBOX_HAS_SYSTEMD_JOURNAL)
    sd_journal* crash_journal{};
#endif
    EventProviderConfiguration configuration{};
    std::uint64_t dropped{};
    bool device_active{};
    bool power_active{};
    bool service_active{};
    bool application_active{};
};

namespace {

[[nodiscard]] bool uevent_requested(const EventProviderConfiguration& configuration) noexcept {
    return configuration.device_events || configuration.audio_device_events ||
           configuration.network_events || configuration.graphics_events ||
           configuration.storage_events;
}

template <typename State>
[[nodiscard]] EventProviderStatus source_status(const EventProviderConfiguration& configuration,
                                                const State& state) noexcept {
    const std::array pairs{
        std::pair{uevent_requested(configuration), state.device_active},
        std::pair{configuration.power_events, state.power_active},
        std::pair{configuration.service_events, state.service_active},
        std::pair{configuration.application_events, state.application_active},
    };
    unsigned requested{};
    unsigned active{};
    for (const auto& [wanted, available] : pairs) {
        requested += static_cast<unsigned>(wanted);
        active += static_cast<unsigned>(wanted && available);
    }
    if (requested == 0U || active == requested) return EventProviderStatus::complete;
    return active == 0U ? EventProviderStatus::temporarily_failed : EventProviderStatus::partial;
}

} // namespace

LinuxSystemEventProvider::LinuxSystemEventProvider() noexcept
    : state_{new (std::nothrow) NativeState{}} {}

LinuxSystemEventProvider::~LinuxSystemEventProvider() { stop(); }

EventProviderStatus
LinuxSystemEventProvider::start(const EventProviderConfiguration& configuration) noexcept {
    stop();
    if (state_ == nullptr) return EventProviderStatus::temporarily_failed;
    state_->configuration = configuration;
    state_->dropped = 0U;
    if (uevent_requested(configuration)) {
        state_->socket =
            ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
        if (state_->socket >= 0) {
            constexpr int receive_buffer_bytes = 256 * 1024;
            static_cast<void>(::setsockopt(state_->socket, SOL_SOCKET, SO_RCVBUF,
                                           &receive_buffer_bytes, sizeof(receive_buffer_bytes)));
            sockaddr_nl address{};
            address.nl_family = AF_NETLINK;
            // Let netlink assign a unique port identifier. A process PID is not
            // a safe socket identity when multiple in-process diagnostic
            // clients exist.
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
    if (configuration.power_events || configuration.service_events) {
        DBusError error;
        dbus_error_init(&error);
        state_->system_bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
        if (state_->system_bus != nullptr) {
            dbus_connection_set_exit_on_disconnect(state_->system_bus, false);
            if (configuration.power_events) {
                DBusError source_error;
                dbus_error_init(&source_error);
                dbus_bus_add_match(state_->system_bus,
                                   "type='signal',sender='org.freedesktop.login1',"
                                   "interface='org.freedesktop.login1.Manager',member='"
                                   "PrepareForSleep'",
                                   &source_error);
                dbus_connection_flush(state_->system_bus);
                if (!dbus_error_is_set(&source_error)) state_->power_active = true;
                dbus_error_free(&source_error);
            }
            if (configuration.service_events) {
                DBusError source_error;
                dbus_error_init(&source_error);
                dbus_bus_add_match(state_->system_bus,
                                   "type='signal',sender='org.freedesktop.systemd1',"
                                   "interface='org.freedesktop.systemd1.Manager',member='"
                                   "JobRemoved'",
                                   &source_error);
                auto* request = dbus_message_new_method_call(
                    "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                    "org.freedesktop.systemd1.Manager", "Subscribe");
                if (request != nullptr && !dbus_error_is_set(&source_error)) {
                    auto* response = dbus_connection_send_with_reply_and_block(
                        state_->system_bus, request, 1'000, &source_error);
                    if (response != nullptr) {
                        state_->service_active = true;
                        dbus_message_unref(response);
                    }
                }
                if (request != nullptr) dbus_message_unref(request);
                dbus_error_free(&source_error);
            }
        }
        dbus_error_free(&error);
    }
#endif

#if defined(BLACKBOX_HAS_SYSTEMD_JOURNAL)
    if (configuration.application_events &&
        sd_journal_open(&state_->crash_journal, SD_JOURNAL_LOCAL_ONLY | SD_JOURNAL_SYSTEM) >= 0 &&
        sd_journal_add_match(state_->crash_journal, "MESSAGE_ID=fc2e22bc6ee647b6b90729ab34a250b1",
                             0U) >= 0 &&
        sd_journal_seek_tail(state_->crash_journal) >= 0) {
        state_->application_active = true;
    } else if (state_->crash_journal != nullptr) {
        sd_journal_close(state_->crash_journal);
        state_->crash_journal = nullptr;
    }
#endif

    return source_status(configuration, *state_);
}

EventProviderPollResult
LinuxSystemEventProvider::poll(const core::MonotonicTimePoint observed_at,
                               const std::span<core::SystemEvent> destination) noexcept {
    if (state_ == nullptr) {
        return {EventProviderStatus::temporarily_failed, 0U, 0U};
    }
    std::size_t count{};

#if defined(BLACKBOX_HAS_DBUS)
    if ((state_->power_active || state_->service_active) && state_->system_bus != nullptr) {
        if (dbus_connection_read_write(state_->system_bus, 0) == 0) {
            state_->power_active = false;
            state_->service_active = false;
        } else {
            while (count < destination.size()) {
                DBusMessage* message = dbus_connection_pop_message(state_->system_bus);
                if (message == nullptr) break;
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
                } else if (dbus_message_is_signal(message, "org.freedesktop.systemd1.Manager",
                                                  "JobRemoved")) {
                    dbus_uint32_t job_id{};
                    const char* object_path{};
                    const char* unit{};
                    const char* result{};
                    DBusError error;
                    dbus_error_init(&error);
                    if (dbus_message_get_args(message, &error, DBUS_TYPE_UINT32, &job_id,
                                              DBUS_TYPE_OBJECT_PATH, &object_path, DBUS_TYPE_STRING,
                                              &unit, DBUS_TYPE_STRING, &result,
                                              DBUS_TYPE_INVALID) != 0 &&
                        unit != nullptr && result != nullptr) {
                        static_cast<void>(job_id);
                        static_cast<void>(object_path);
                        if (std::string_view{unit}.ends_with(".service")) {
                            auto event = normalized_linux_service_job_event(result);
                            event.observed_at = observed_at;
                            destination[count++] = event;
                        }
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

#if defined(BLACKBOX_HAS_SYSTEMD_JOURNAL)
    if (state_->application_active && state_->crash_journal != nullptr &&
        count < destination.size()) {
        const auto process_result = sd_journal_process(state_->crash_journal);
        if (process_result < 0) {
            state_->application_active = false;
        } else {
            while (count < destination.size()) {
                const auto next = sd_journal_next(state_->crash_journal);
                if (next <= 0) {
                    if (next < 0) state_->application_active = false;
                    break;
                }
                auto event = normalized_linux_application_crash_event();
                event.observed_at = observed_at;
                destination[count++] = event;
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
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                if (errno == ENOBUFS) {
                    ++state_->dropped;
                    break;
                }
                state_->device_active = false;
                break;
            }
            if (received == 0) break;
            if (static_cast<std::size_t>(received) > buffer.size()) {
                ++state_->dropped;
                continue;
            }
            auto event = normalized_linux_uevent(
                std::string_view{buffer.data(), static_cast<std::size_t>(received)},
                state_->configuration);
            if (!event) continue;
            event->observed_at = observed_at;
            destination[count++] = *event;
        }
    }
    return {source_status(state_->configuration, *state_), count, state_->dropped};
}

void LinuxSystemEventProvider::stop() noexcept {
    if (state_ == nullptr) return;
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
#if defined(BLACKBOX_HAS_SYSTEMD_JOURNAL)
    if (state_->crash_journal != nullptr) {
        sd_journal_close(state_->crash_journal);
        state_->crash_journal = nullptr;
    }
#endif
    state_->device_active = false;
    state_->power_active = false;
    state_->service_active = false;
    state_->application_active = false;
}

EventProviderCapabilities LinuxSystemEventProvider::capabilities() const noexcept {
    EventProviderCapabilities result{};
    result.device_events = true;
#if defined(BLACKBOX_HAS_DBUS)
    result.power_events = true;
    result.service_events = true;
#endif
    result.audio_device_events = true;
    result.network_events = true;
    result.graphics_events = true;
    result.storage_events = true;
#if defined(BLACKBOX_HAS_SYSTEMD_JOURNAL)
    result.application_events = true;
#endif
    return result;
}

} // namespace blackbox::telemetry::linux
