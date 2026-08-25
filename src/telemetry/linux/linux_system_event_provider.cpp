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

namespace blackbox::telemetry::linux {

struct LinuxSystemEventProvider::NativeState {
    int socket{-1};
    EventProviderConfiguration configuration{};
    std::uint64_t dropped{};
};

LinuxSystemEventProvider::LinuxSystemEventProvider() noexcept
    : state_{new (std::nothrow) NativeState{}} {}

LinuxSystemEventProvider::~LinuxSystemEventProvider() {
    stop();
}

EventProviderStatus LinuxSystemEventProvider::start(
    const EventProviderConfiguration& configuration) noexcept {
    stop();
    if (state_ == nullptr) return EventProviderStatus::temporarily_failed;
    state_->configuration = configuration;
    state_->dropped = 0U;
    if (!configuration.device_events) return EventProviderStatus::complete;

    state_->socket = ::socket(AF_NETLINK,
                              SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                              NETLINK_KOBJECT_UEVENT);
    if (state_->socket < 0) return EventProviderStatus::temporarily_failed;

    constexpr int receive_buffer_bytes = 256 * 1024;
    static_cast<void>(::setsockopt(state_->socket, SOL_SOCKET, SO_RCVBUF,
                                   &receive_buffer_bytes,
                                   sizeof(receive_buffer_bytes)));
    sockaddr_nl address{};
    address.nl_family = AF_NETLINK;
    // Let netlink assign a unique port identifier. A process PID is not a
    // safe socket identity when multiple in-process diagnostic clients exist.
    address.nl_pid = 0U;
    address.nl_groups = 1U;
    if (::bind(state_->socket, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
        stop();
        return EventProviderStatus::temporarily_failed;
    }
    return EventProviderStatus::complete;
}

EventProviderPollResult LinuxSystemEventProvider::poll(
    const core::MonotonicTimePoint observed_at,
    const std::span<core::SystemEvent> destination) noexcept {
    if (state_ == nullptr) {
        return {EventProviderStatus::temporarily_failed, 0U, 0U};
    }
    if (!state_->configuration.device_events) {
        return {EventProviderStatus::complete, 0U, state_->dropped};
    }
    if (state_->socket < 0) {
        return {EventProviderStatus::temporarily_failed, 0U, state_->dropped};
    }

    std::array<char, 4096U> buffer{};
    std::size_t count{};
    while (count < destination.size()) {
        const auto received = ::recv(state_->socket, buffer.data(), buffer.size(),
                                     MSG_DONTWAIT | MSG_TRUNC);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            if (errno == ENOBUFS) {
                ++state_->dropped;
                break;
            }
            return {EventProviderStatus::temporarily_failed, count, state_->dropped};
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
    return {EventProviderStatus::complete, count, state_->dropped};
}

void LinuxSystemEventProvider::stop() noexcept {
    if (state_ != nullptr && state_->socket >= 0) {
        static_cast<void>(::close(state_->socket));
        state_->socket = -1;
    }
}

EventProviderCapabilities LinuxSystemEventProvider::capabilities() const noexcept {
    EventProviderCapabilities result{};
    result.device_events = true;
    return result;
}

} // namespace blackbox::telemetry::linux
